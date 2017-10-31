#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <regex.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "srinagar.h"

int main(int argc, char **argv)
{
	if (argc != 3)
	{
		puts("Usage:\n");
		printf("%s <address to bind> <port>\n", argv[0]);
		return 1;
	}
	const char *address = argv[1];
	const char *port = argv[2];
	int status = run_server(address, port);
	return status;
}

int run_server(const char *address, const char *port)
{
	struct addrinfo hints;
	struct addrinfo *address_info;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	int status = getaddrinfo(address, port, &hints, &address_info);
	if (status != 0)
	{
		perror("getaddrinfo");
		return -1;
	}
	int server_fd = socket(address_info->ai_family, address_info->ai_socktype, address_info->ai_protocol);
	if (server_fd == -1)
	{
		perror("socket");
		freeaddrinfo(address_info);
		return -1;
	}
	int enable = 1;
	status = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
	if (status != 0)
	{
		perror("setsockopt");
		return -1;
	}
	status = bind(server_fd, address_info->ai_addr, address_info->ai_addrlen);
	freeaddrinfo(address_info);
	if (status != 0)
	{
		perror("bind");
		return -1;
	}
	status = enable_non_blocking_mode(server_fd);
	if (status == -1)
	{
		return -1;
	}
	status = listen(server_fd, SOMAXCONN);
	if (status == -1)
	{
		perror("listen");
		return -1;
	}
	int epoll_fd = epoll_create1(0);
	if (epoll_fd == -1)
	{
		perror("epoll_create1");
		return -1;
	}
	struct epoll_event event;
	event.data.fd = server_fd;
	event.events = EPOLLIN | EPOLLET;
	status = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);
	if (status == -1)
	{
		perror("epoll_ctl (server)");
		return -1;
	}
	printf("Running HTTP server on %s:%s\n", address, port);
	struct epoll_event *events = calloc(maximum_events, sizeof(struct epoll_event));
	while (1)
	{
		status = process_epoll_event(server_fd, epoll_fd, events);
		if (status != 0)
		{
			return -1;
		}
	}
}

int process_epoll_event(int server_fd, int epoll_fd, struct epoll_event *events)
{
	int ready_count = epoll_wait(epoll_fd, events, maximum_events, -1);
	if (ready_count == -1)
	{
		perror("epoll_wait");
		return -1;
	}
	int status;
	for (int i = 0; i < ready_count; i++)
	{
		struct epoll_event *event = events + i;
		int event_fd = event->data.fd;
		status = 0;
		if (event->events & EPOLLERR || event->events & EPOLLHUP || !(event->events & EPOLLIN))
		{
			close(event_fd);
		}
		else if (event_fd == server_fd)
		{
			status = on_connect(server_fd, epoll_fd);
		}
		else
		{
			status = on_receive(event_fd);
		}
		if (status != 0)
		{
			return -1;
		}
	}
	return 0;
}

int enable_non_blocking_mode(int server_fd)
{
	int flags = fcntl(server_fd, F_GETFL, 0);
	if (flags == -1)
	{
		perror("fcntl (F_GETFL)");
		return -1;
	}
	int new_flags = flags | O_NONBLOCK;
	int status = fcntl(server_fd, F_SETFL, new_flags);
	if (status == -1)
	{
		perror("fcntl (F_SETFL)");
		return -1;
	}
	return 0;
}

int on_connect(int server_fd, int epoll_fd)
{
	while (1)
	{
		struct sockaddr client_address;
		socklen_t client_address_size = sizeof(client_address);
		int client_fd = accept(server_fd, &client_address, &client_address_size);
		if (client_fd == -1)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				break;
			}
			perror("accept");
			return -1;
		}
		int status = enable_non_blocking_mode(client_fd);
		if (status != 0)
		{
			return -1;
		}
		struct epoll_event new_event;
		new_event.data.fd = client_fd;
		new_event.events = EPOLLIN | EPOLLET;
		status = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &new_event);
		if (status == -1)
		{
			perror("epoll_ctl (client)");
			return -1;
		}
		char host_buffer[NI_MAXHOST];
		char port_buffer[NI_MAXSERV];
		status = getnameinfo(&client_address, sizeof(client_address), host_buffer, sizeof(host_buffer), port_buffer, sizeof(port_buffer), NI_NUMERICHOST | NI_NUMERICSERV);
		if (status == 0)
		{
			printf("New connection from %s:%s\n", host_buffer, port_buffer);
		}
		else
		{
			perror("getnameinfo");
		}
	}
	return 0;
}

int on_receive(int client_fd)
{
	char buffer[buffer_size];
	memset(buffer, 0, sizeof(buffer));
	size_t bytes_read = recv(client_fd, buffer, sizeof(buffer), MSG_PEEK);
	if (bytes_read == 0)
	{
		close(client_fd);
	}
	else if (bytes_read == sizeof(buffer))
	{
		close(client_fd);
	}
	else if (bytes_read == -1)
	{
		perror("recv");
		close(client_fd);
	}
	else
	{
		regex_t regex;
		int status = regcomp(&regex, "^GET (.+?) HTTP/1.1\r\n.+?\r\n\r\n$", REG_EXTENDED);
		if (status != 0)
		{
			char error_message[buffer_size];
			regerror(status, &regex, error_message, sizeof(error_message));
			fprintf(stderr, "regcomp: %s", error_message);
			return -1;
		}
		size_t group_count = 2;
		regmatch_t matches[group_count];
		status = regexec(&regex, buffer, group_count, matches, 0);
		if (status == 0)
		{
			char path[buffer_size];
			memset(path, 0, sizeof(path));
			regmatch_t *match = matches + 1;
			memcpy(path, buffer + match->rm_so, match->rm_eo - match->rm_so);
			recv(client_fd, buffer, sizeof(buffer), 0);
			char header[buffer_size];
			char *body = path;
			size_t body_size = strlen(path);
			int header_size = snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: keep-alive\r\n\r\n", body_size);
			send(client_fd, header, header_size, 0);
			send(client_fd, body, body_size, 0);
		}
		regfree(&regex);
	}
}