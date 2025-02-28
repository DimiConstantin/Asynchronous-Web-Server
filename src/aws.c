// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the reply header. */
	snprintf(conn->send_buffer, sizeof(conn->send_buffer),
			"HTTP/1.1 200 OK\r\n"
			"Content-Length: %ld\r\n"
			"Connection: close\r\n"
			"\r\n",
			conn->file_size);
	conn->send_len = strlen(conn->send_buffer);
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* TODO: Prepare the connection buffer to send the 404 header. */
	snprintf(conn->send_buffer, sizeof(conn->send_buffer),
		"HTTP/1.1 404 Not Found\r\n"
		"Content-Length: 0\r\n"
		"Connection: close\r\n"
		"\r\n");

	conn->send_len = strlen(conn->send_buffer);
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* TODO: Get resource type depending on request path/filename. Filename should
	 * point to the static or dynamic folder.
	 */
	if (strstr(conn->filename, "static") != NULL)
		return RESOURCE_TYPE_STATIC;
	if (strstr(conn->filename, "dynamic") != NULL)
		return RESOURCE_TYPE_DYNAMIC;
	return RESOURCE_TYPE_NONE;
}



struct connection *connection_create(int sockfd)
{
	/* TODO: Initialize connection structure on given socket. */
	struct connection *conn = calloc(1, sizeof(struct connection));

	DIE(!conn, "connection failed");

	conn->sockfd = sockfd;
	dlog(LOG_INFO, "sockfd: %d\n", sockfd);
	conn->state = STATE_RECEIVING_DATA;
	io_setup(1, &conn->ctx);

	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	/* TODO: Start asynchronous operation (read from file).
	 * Use io_submit(2) & friends for reading data asynchronously.
	 */
}

void connection_remove(struct connection *conn)
{
	/* TODO: Remove connection handler. */
	if (conn->fd > 0)
		close(conn->fd);

	if (conn->sockfd > 0)
		close(conn->sockfd);

	w_epoll_remove_fd(epollfd, conn->sockfd);

	conn->state = STATE_CONNECTION_CLOSED;
	free(conn);
}

void handle_new_connection(void)
{
	/* TODO: Handle a new connection request on the server socket. */
	int client_fd;
	struct connection *conn;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	struct sockaddr_in client_addr;
	int rc;

	/* TODO: Accept new connection. */
	client_fd = accept(listenfd, (struct sockaddr *)&client_addr, &addrlen);
	DIE(client_fd < 0, "accept");

	/* TODO: Set socket to be non-blocking. */
	int flags = fcntl(client_fd, F_GETFL, 0);

	fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

	/* TODO: Instantiate new connection handler. */
	conn = connection_create(client_fd);

	/* TODO: Add socket to epoll. */
	rc = w_epoll_add_ptr_in(epollfd, client_fd, conn);
	DIE(rc < 0, "w_epoll_add_ptr_in");

	/* TODO: Initialize HTTP_REQUEST parser. */
	http_parser_init(&conn->request_parser, HTTP_REQUEST);
	conn->request_parser.data = conn;
}

void receive_data(struct connection *conn)
{
	/* TODO: Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */

	ssize_t bytes_read;
	char aux[BUFSIZ];

	bytes_read = recv(conn->sockfd, aux, BUFSIZ, 0);
	if (bytes_read < 0) {
		ERR("recv");
		connection_remove(conn);
		return;
	}

	memcpy(conn->recv_buffer + conn->recv_len, aux, bytes_read);
	conn->recv_len += bytes_read;
	conn->recv_buffer[conn->recv_len] = '\0';

	if (strstr(conn->recv_buffer, "\r\n\r\n") != NULL) {
		parse_header(conn);

		dlog(LOG_INFO, "Filename: %s\n", conn->filename);

		conn->state = STATE_REQUEST_RECEIVED;
		w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
	}
}

int connection_open_file(struct connection *conn)
{
	/* TODO: Open file and update connection fields. */

	conn->fd = open(conn->filename, O_RDONLY);

	struct stat file_stat;

	fstat(conn->fd, &file_stat);

	conn->file_size = file_stat.st_size;

	return conn->fd;
}

void connection_complete_async_io(struct connection *conn)
{
	/* TODO: Complete asynchronous operation; operation returns successfully.
	 * Prepare socket for sending.
	 */
}

int parse_header(struct connection *conn)
{
	/* TODO: Parse the HTTP header and extract the file path. */
	/* Use mostly null settings except for on_path callback. */
	http_parser_settings settings_on_path = {
		.on_message_begin = 0,
		.on_header_field = 0,
		.on_header_value = 0,
		.on_path = aws_on_path_cb,
		.on_url = 0,
		.on_fragment = 0,
		.on_query_string = 0,
		.on_body = 0,
		.on_headers_complete = 0,
		.on_message_complete = 0
	};
	(void) settings_on_path;

	char *HTTP_pos = strstr(conn->recv_buffer, " HTTP");
	char filename[BUFSIZ];

	filename[0] = '.';
	filename[1] = '/';

	int n = 2;

	char *ptr = conn->recv_buffer + 5;

	while (ptr != HTTP_pos) {
		filename[n++] = *ptr;
		ptr++;
	}

	strcpy(conn->filename, filename);
	dlog(LOG_INFO, "Filename: %s\n", conn->filename);
	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* TODO: Send static data using sendfile(2). */

	// sending file


	int sent_bytes = sendfile(conn->sockfd, conn->fd, NULL, conn->file_size);

	if (sent_bytes <= 0)
		return STATE_DATA_SENT;

	conn->file_pos += sent_bytes;

	if (conn->file_pos == conn->file_size)
		return STATE_DATA_SENT;

	return STATE_SENDING_DATA;
}

int connection_send_data(struct connection *conn)
{
	/* May be used as a helper function. */
	/* TODO: Send as much data as possible from the connection send buffer.
	 * Returns the number of bytes sent or -1 if an error occurred
	 */
	return -1;
}


int connection_send_dynamic(struct connection *conn)
{
	/* TODO: Read data asynchronously.
	 * Returns 0 on success and -1 on error.
	 */

	int rc;
	struct io_event event;
	int sent_bytes;

	switch (conn->state) {
	case STATE_HEADER_SENT:

		conn->send_pos = 0;
		struct iocb iocb;

		io_prep_pread(&iocb, conn->fd, conn->send_buffer, BUFSIZ, conn->file_pos);
		conn->piocb[0] = &iocb;

		rc = io_submit(conn->ctx, 1, conn->piocb);

		if (rc < 0) {
			dlog(LOG_INFO, "io_submit failed\n");
			return -1;
		}

		conn->state = STATE_ASYNC_ONGOING;
		break;
	case STATE_ASYNC_ONGOING:

		rc = io_getevents(conn->ctx, 1, 1, &event, NULL);
		DIE(rc < 0, "io_getevents");

		conn->state = STATE_SENDING_DATA;
		break;
	case STATE_SENDING_DATA:
		sent_bytes = send(conn->sockfd, conn->send_buffer + conn->send_pos, BUFSIZ, 0);

		if (sent_bytes > 0) {
			conn->send_pos += sent_bytes;
			conn->file_pos += sent_bytes;
			conn->file_size -= sent_bytes;
		}

		if (sent_bytes <= 0 || conn->file_size == 0) {
			conn->state = STATE_DATA_SENT;
			return 0;
		}

		if (conn->send_pos >= BUFSIZ)
			conn->state = STATE_HEADER_SENT;
		break;
	default:
		break;
	}

	return 0;
}


void handle_input(struct connection *conn)
{
	/* TODO: Handle input information: may be a new message or notification of
	 * completion of an asynchronous I/O operation.
	 */

	switch (conn->state) {
	case STATE_RECEIVING_DATA:
		receive_data(conn);
		return;
	default:
		break;
	}
}

void handle_output(struct connection *conn)
{
	/* TODO: Handle output information: may be a new valid requests or notification of
	 * completion of an asynchronous I/O operation or invalid requests.
	 */

	int sent_bytes;

	switch (conn->state) {
	case STATE_REQUEST_RECEIVED:
		if (connection_open_file(conn) < 0) {
			conn->state = STATE_SENDING_404;
			dlog(LOG_INFO, "404\n");
			break;
		}
		dlog(LOG_INFO, "s a deschis fisierul\n");
		connection_prepare_send_reply_header(conn);
		conn->state = STATE_SENDING_HEADER;
		break;
	case STATE_SENDING_HEADER:

		sent_bytes = send(conn->sockfd, conn->send_buffer + conn->send_pos, conn->send_len - conn->send_pos, 0);
		conn->send_pos += sent_bytes;

		if (sent_bytes <= 0)
			conn->state = STATE_HEADER_SENT;
		break;

	case STATE_HEADER_SENT:
	case STATE_ASYNC_ONGOING:
	case STATE_SENDING_DATA:
		if (connection_get_resource_type(conn) == RESOURCE_TYPE_STATIC)
			conn->state = connection_send_static(conn);
		else
			connection_send_dynamic(conn);
		break;
	case STATE_DATA_SENT:
		connection_remove(conn);
		break;
	case STATE_SENDING_404:
		connection_prepare_send_404(conn);

		sent_bytes = send(conn->sockfd, conn->send_buffer + conn->send_pos, conn->send_len - conn->send_pos, 0);
		conn->send_pos += sent_bytes;

		if (sent_bytes == 0)
			conn->state = STATE_DATA_SENT;
		break;
	case STATE_CONNECTION_CLOSED:
		break;
	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void handle_client(uint32_t event, struct connection *conn)
{
	/* TODO: Handle new client. There can be input and output connections.
	 * Take care of what happened at the end of a connection.
	 */
	if (event & EPOLLIN)
		handle_input(conn);
	else if (event & EPOLLOUT)
		handle_output(conn);
}

int main(void)
{
	int rc;

	/* Initialize asynchronous operations. */

	/* Initialize multiplexing. */
	epollfd = w_epoll_create();

	/* Create server socket. */
	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);

	rc = w_epoll_add_fd_in(epollfd, listenfd);

	/* Uncomment the following line for debugging. */
	dlog(LOG_INFO, "Server waiting for connections on port %d\n", AWS_LISTEN_PORT);

	/* server main loop */
	while (1) {
		struct epoll_event rev;

		/* Wait for events. */
		rc = w_epoll_wait_infinite(epollfd, &rev);
		DIE(rc < 0, "w_epoll_wait_infinite");


		/* Switch event types; consider
		 *   - new connection requests (on server socket)
		 *   - socket communication (on connection sockets)
		 */
		if (rev.data.fd == listenfd) {
			if (rev.events & EPOLLIN)
				handle_new_connection();
		} else {
			handle_client(rev.events, rev.data.ptr);
		}
	}

	return 0;
}
