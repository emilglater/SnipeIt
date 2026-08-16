#include "unix_socket.h"

/* Standard Libraries */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

// Set socket to non-blocking mode
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        perror("fcntl F_GETFL");
        return -1;
    }
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        perror("fcntl F_SETFL");
        return -1;
    }
    
    return 0;
}

// Send all data (handles partial sends)
static int sendall(int fd, const char *buf, size_t len)
{
    size_t total = 0;
    size_t bytesleft = len;
    ssize_t n;
    
    while (total < len)
    {
        n = send(fd, buf + total, bytesleft, 0);
        if (n == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // Socket buffer full, try again
                usleep(1000);   // Wait 1ms
                continue;
            }
            perror("send");
            return -1;
        }
        total += n;
        bytesleft -= n;
    }
    
    return 0;
}

int ipc_server_init(IPCConnection *conn)
{
    // Initialize structure
    *conn = (IPCConnection){
        .server_fd = -1,
        .client_fd = -1,
        .recv_buffer = { 0 },
        .recv_len = 0
    };
    
    // Remove existing socket file if present
    unlink(SOCKET_PATH);
    
    // Create socket - AF_UNIX for Unix domain socket
    conn->server_fd = socket(PF_UNIX, SOCK_STREAM, 0);
    if (conn->server_fd == -1)
    {
        perror("socket");
        return -1;
    }

    struct sockaddr_un addr = {
        .sun_family = AF_UNIX
    };
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);
    
    // Bind socket to the path
    if (bind(conn->server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("bind");
        close(conn->server_fd);
        conn->server_fd = -1;
        return -1;
    }
    
    // Set socket file permissions (allow all local users)
    chmod(SOCKET_PATH, 0666);
    
    // Listen for connections (backlog of 1 - only one Python client)
    if (listen(conn->server_fd, 1) == -1) {
        perror("listen");
        close(conn->server_fd);
        unlink(SOCKET_PATH);
        conn->server_fd = -1;
        return -1;
    }
    
    printf("[IPC] Server listening on %s\n", SOCKET_PATH);
    return 0;
}

int ipc_accept_client(IPCConnection *conn)
{
    struct sockaddr_un client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    if (conn->server_fd == -1)
    {
        fprintf(stderr, "[IPC] Server not initialized\n");
        return -1;
    }
    
    // Close existing client if there is any
    if (conn->client_fd != -1)
    {
        close(conn->client_fd);
        conn->client_fd = -1;
    }
    
    // Accept new connection (blocking)
    conn->client_fd = accept(conn->server_fd, (struct sockaddr*)&client_addr, &client_addr_len);
    if (conn->client_fd == -1)
    {
        perror("accept");
        return -1;
    }

    // Increase socket receive buffer to 1MB for high-throughput IPC
    int rcvbuf = 1024 * 1024;
    if (setsockopt(conn->client_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) == -1)
    {
        perror("setsockopt SO_RCVBUF");
        // Non-fatal, continue anyway
    }

    // Set client socket to non-blocking for recv
    if (set_nonblocking(conn->client_fd) == -1)
    {
        close(conn->client_fd);
        conn->client_fd = -1;
        return -1;
    }
    
    // Clear receive buffer
    conn->recv_len = 0;
    conn->recv_buffer[0] = '\0';
    
    printf("[IPC] Python client connected\n");
    return 0;
}

int ipc_check_client_connected(IPCConnection *conn)
{
    return (conn->client_fd != -1) ? 1 : 0;
}

int ipc_send_start(IPCConnection *conn, const char *video_path, double duration_sec,
                    double fps, int loop, int frame_interval)
{
    char msg[MAX_MSG_SIZE];
    int len;
    
    if (conn->client_fd == -1)
    {
        fprintf(stderr, "[IPC] No client connected\n");
        return -1;
    }
    
    // Format JSON message with newline delimiter
    len = snprintf(msg, sizeof(msg),
        "{\"cmd\":\"start\",\"video_path\":\"%s\",\"duration_sec\":%.3f,"
        "\"fps\":%.2f,\"loop\":%s,\"frame_interval\":%d}\n",
        video_path, duration_sec, fps, loop ? "true" : "false", frame_interval);
    
    if (len >= (int)sizeof(msg))
    {
        fprintf(stderr, "[IPC] Message too long\n");
        return -1;
    }
    
    printf("[IPC] Sending START command: %s", msg);
    return sendall(conn->client_fd, msg, len);
}

int ipc_send_stop(IPCConnection *conn)
{
    const char *msg = "{\"cmd\":\"stop\"}\n";
    
    if (conn->client_fd == -1)
    {
        fprintf(stderr, "[IPC] No client connected\n");
        return -1;
    }
    
    printf("[IPC] Sending STOP command\n");
    return sendall(conn->client_fd, msg, strlen(msg));
}

int ipc_recv_message(IPCConnection *conn, char *buffer, size_t buffsize)
{
    char *newline;
    ssize_t bytes_read;
    size_t msg_len;
    
    if (conn->client_fd == -1)
    {
        return -1;
    }
    
    // Try to receive more data into buffer
    if (conn->recv_len < sizeof(conn->recv_buffer) - 1)
    {
        bytes_read = recv(conn->client_fd, conn->recv_buffer + conn->recv_len,
                 sizeof(conn->recv_buffer) - conn->recv_len - 1, 0);

        if (bytes_read > 0)
        {
            conn->recv_len += bytes_read;
            conn->recv_buffer[conn->recv_len] = '\0';
        }
        else if (bytes_read == 0)
        {
            // Client disconnected
            printf("[IPC] Python client disconnected\n");
            close(conn->client_fd);
            conn->client_fd = -1;
            return -1;
        }
        // EAGAIN/EWOULDBLOCK mean no data available - that's OK
        else if (bytes_read < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            perror("recv");
            close(conn->client_fd);
            conn->client_fd = -1;
            return -1;
        }
    }
    
    // Check if we have a complete message (ending with newline)
    newline = strchr(conn->recv_buffer, '\n');
    if (newline == NULL) {
        // No complete message yet
        return 0;
    }
    
    // Extract the message (without newline)
    msg_len = newline - conn->recv_buffer;
    if (msg_len >= buffsize) {
        fprintf(stderr, "[IPC] Message too large for buffer\n");
        // Discard the message
        msg_len = buffsize - 1;
    }
    
    memcpy(buffer, conn->recv_buffer, msg_len);
    buffer[msg_len] = '\0';
    
    // Remove processed message from receive buffer
    size_t remaining = conn->recv_len - (msg_len + 1);  // +1 for newline
    if (remaining > 0)
    {
        memmove(conn->recv_buffer, newline + 1, remaining);
    }
    conn->recv_len = remaining;
    conn->recv_buffer[conn->recv_len] = '\0';
    
    return msg_len;
}

int ipc_get_server_fd(IPCConnection *conn)
{
    return conn->server_fd;
}

int ipc_get_client_fd(IPCConnection *conn)
{
    return conn->client_fd;
}

void ipc_disconnect_client(IPCConnection *conn)
{
    if (conn->client_fd != -1) {
        close(conn->client_fd);
        conn->client_fd = -1;
        conn->recv_len = 0;
        printf("[IPC] Client disconnected\n");
    }
}

void ipc_cleanup(IPCConnection *conn)
{
    if (conn->client_fd != -1) {
        close(conn->client_fd);
        conn->client_fd = -1;
    }
    
    if (conn->server_fd != -1) {
        close(conn->server_fd);
        conn->server_fd = -1;
    }
    
    // Remove socket file
    unlink(SOCKET_PATH);
    
    printf("[IPC] Cleanup complete\n");
}