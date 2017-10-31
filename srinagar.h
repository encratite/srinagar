#pragma once

enum
{
	maximum_events = 64,
	buffer_size = 4096
};

int run_server(const char *address, const char *port);
int process_epoll_event(int server_fd, int epoll_fd, struct epoll_event *events);
int enable_non_blocking_mode(int server_fd);
int on_connect(int server_fd, int epoll_fd);
int on_receive(int client_fd);