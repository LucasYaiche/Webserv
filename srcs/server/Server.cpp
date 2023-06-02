#include "Server.hpp"


Server::Server(std::vector<ServInfo> ports) : _ports(ports)
{   
    for(size_t i=0; i < ports.size(); i++)
    {
        Socket socket;
        socket.create_socket();
        socket.set_non_blocking();
        if (!socket.bind("127.0.0.1", ports[i].getPort())) 
        {
            throw std::runtime_error("Failed to bind server socket.");
        }

        if (!socket.listen(42)) // A voir si on peut pas le preset dans les fichiers de config
        {
            std::cout << "listen error" << std::endl;
            throw std::runtime_error("Failed to listen on server socket.");
        }

        _server_sockets.push_back(socket);

        // Add the server socket to the fds vector with the POLLIN event
        pollfd server_pollfd;
        server_pollfd.fd = socket.get_fd();
        server_pollfd.events = POLLIN;
        server_pollfd.revents = 0;
        _fds.push_back(server_pollfd);
    }
}

Server::~Server() {}

void Server::close_sockets() 
{
        for (size_t i = 0; i < _server_sockets.size(); ++i) {
            if (_server_sockets[i].get_fd() != -1) {
                _server_sockets[i].close();
            }
        }
        for (size_t i = 0; i < _fds.size(); ++i) {
            Socket client_socket;
            client_socket.set_socket_fd(_fds[i].fd);
            if (client_socket.get_fd() != -1) {
                client_socket.close();
            }
        }
    }

std::pair<bool, Location>   Server::check_location(ServInfo& current_port, const std::string& request_location) 
{
    for (size_t i = 0; i < current_port.getLocation().size(); i++) {
        if (current_port.getLocation()[i].getPath() == request_location) {
            return std::make_pair(true, current_port.getLocation()[i]);
        }
    }
    // Return a default Location if not found
    return std::make_pair(false, Location());
}

bool Server::is_method_valid(std::pair<bool, Location> result, const std::string& method) 
{
    if (method == "POST" || method == "GET" || method == "DELETE")
    {
        return true;
    }
    
    if(result.first)
    {
        Location port_location = result.second;

        for (size_t i = 0; i < port_location.getMethods().size(); i++) 
        {
            if (port_location.getMethods()[i] == method) 
            {
                return true;
            }
        }
    }
    return false;
}

void    Server::delete_socket(Socket client_socket, size_t i)
{
    client_socket.close();
    _fds.erase(_fds.begin() + i);
    i--;
    delete[] _buffer;
}


void Server::run() 
{
    while (true) 
    {
        int poll_result = poll(&_fds[0], _fds.size(), -1);

        if (poll_result <= 0) 
        {
            std::cerr << "poll() failed: " << std::endl;
            if (!poll_result)
            {
                std::cerr << "poll time out" << std::endl;
            } 
            continue;
        }

        for (size_t i = 0; i < _fds.size(); ++i) 
        {
            if (_fds[i].revents & POLLIN) 
            {
                bool is_server_fd = false;
                for(size_t j = 0; j < _server_sockets.size(); j++)
                {
                    if (_fds[i].fd == _server_sockets[j].get_fd()) 
                    {
                        is_server_fd = true;

                        sockaddr_in client_address;
                        socklen_t client_addr_len = sizeof(client_address);
                        int client_socket_fd = _server_sockets[j].accept((sockaddr*)&client_address, &client_addr_len);
                        if (client_socket_fd == -1)
                        {
                            std::cerr << "accept() failed: " << std::endl;
                            continue;
                        }
                        else
                        {
                            Socket client_socket;
                            client_socket.set_socket_fd(client_socket_fd);
                            if (client_socket.set_non_blocking() == -1)
                                continue;

                            int n = 1;
                            if (setsockopt(client_socket.get_fd(), SOL_SOCKET, SO_REUSEADDR, &n, sizeof(n)) == -1)
                            {
                                std::cerr << "setsockopt socket error" << std::endl;
                                continue;
                            }

                            pollfd client_pollfd;
                            client_pollfd.fd = client_socket_fd;
                            client_pollfd.events = POLLIN;
                            client_pollfd.revents = 0;
                            _fds.push_back(client_pollfd);
                        }
                        break;
                    }
                }

                if (!is_server_fd)
                {
                    Socket client_socket;
                    client_socket.set_socket_fd(_fds[i].fd);
                    ssize_t bytes_received = 0;
                    int port_nb = client_socket.get_local_port();
                    ServInfo current_port;
                    for(size_t i=0; i < _ports.size(); i++)
                    {
                        if(_ports[i].getPort() == port_nb)
                        {
                            current_port = _ports[i];
                            break;
                        }
                    }

                    _buffer_size = current_port.getBody_size();
                    _buffer = new char[_buffer_size];

                    if (!_buffer)
                    {
                        send_error_response(client_socket.get_fd(), 500, "Internal Server Error");
                        delete_socket(client_socket, i);
                        continue;
                    }

                    bytes_received = client_socket.recv(_buffer, _buffer_size);

                    if (bytes_received <= 0) 
                    {
                        send_error_response(client_socket.get_fd(), 500, "Internal Server Error");
                        delete_socket(client_socket, i);
                        continue;
                    }

                    Request request;
                    if (request.parse(_buffer, bytes_received) == -1)
                    {
                        send_error_response(client_socket.get_fd(), 400, "Bad Request");
                        delete_socket(client_socket, i);
                        continue;
                    }
                        
                    std::pair<bool, Location> result = check_location(current_port, request.get_uri());

                    std::string method = request.get_method();
                    bool methodValid = is_method_valid(result, method);

                    if (request.is_cgi() && methodValid)
                    {
                        if (handle_cgi_request(_fds[i].fd, request, _ports) == -1)
                        {
                            delete[] _buffer;
                            continue;
                        }
                    }
                    else if(methodValid)
                    {    
                        if (method == "GET") 
                        {
                            if (handle_get_request(_fds[i].fd, request, current_port.getRoot()) == -1)
                            {
                                delete[] _buffer;
                                continue;
                            }
                        }
                        else if (method == "POST") 
                        {
                            if (handle_post_request(_fds[i].fd, request, current_port.getRoot()) == -1)
                            {
                                delete[] _buffer;
                                continue;
                            }
                        }
                        else if (method == "DELETE") 
                        {
                            if (handle_delete_request(_fds[i].fd, request, current_port.getRoot()) == -1)
                            {
                                delete[] _buffer;
                                continue;
                            }
                        }
                    }
                    else 
                    {
                        send_error_response(client_socket.get_fd(), 405, "Method not allowed");
                        delete[] _buffer;
                        continue;
                    }

                    if (client_socket.send(_buffer, bytes_received) == -1) {
                        delete[] _buffer;
                        continue;
                    }

                    delete[] _buffer;
                }
            }
            usleep(5000);
        }
    }
    close_sockets();
}

int Server::handle_cgi_request(int client_fd, const Request& request, std::vector<ServInfo> ports)
{
    // Extract the path and the query string from the URI
    std::string uri = request.get_uri();
    std::size_t delimiter_pos = uri.find('?');
    std::string path = uri.substr(0, delimiter_pos);
    std::string query_string = delimiter_pos == std::string::npos ? "" : uri.substr(delimiter_pos + 1);

    Socket client_socket;
    client_socket.set_socket_fd(client_fd);
    
    // Create a new CGI instance and run the script
    CGI cgi(path, query_string, request, ports, client_fd);
    std::string script_output = cgi.run_cgi_script();

    // Send the script output back to the client
    client_socket.set_non_blocking();
    if (client_socket.send(script_output.c_str(), script_output.size()) == -1) {
        std::cerr << "Error: could not send data\n";
        return -1;
    }
    return 0;
}
