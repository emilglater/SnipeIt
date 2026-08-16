/*
 * test_ipc.c
 * 
 * Test program for Unix Domain Socket IPC
 * 
 * This simulates the C server side:
 * 1. Starts the IPC server
 * 2. Waits for Python client to connect
 * 3. Sends a START command
 * 4. Receives detection messages
 * 5. Sends a STOP command
 * 6. Cleans up
 * 
 * Compile:
 *   gcc -o test_ipc test_ipc.c unix_socket.c -Wall
 * 
 * Run:
 *   ./test_ipc
 *   (Then run test_ipc.py in another terminal)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>

#include "unix_socket.h"

#define VIDEO_TEST_PATH "~/SnipeIt/videos/duck_slide.mp4"
#define FRAMES_REQUIRED 10

// Global for signal handler
static volatile int running = 1;

// Signal handler for clean shutdown
void handle_signal(int sig)
{
    (void)sig;
    printf("\n[TEST] Received signal, shutting down...\n");
    running = 0;
}

int main(int argc, char *argv[])
{
    IPCConnection conn;
    char buffer[MAX_MSG_SIZE];
    int msg_count = 0;
    const char *video_path = VIDEO_TEST_PATH;
    
    // Allow custom video path from command line
    if (argc > 1)
    {
        video_path = argv[1];
    }
    
    // Set up signal handlers
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    
    printf("IPC Test - C Server\n");
    
    // Initialize server
    printf("[TEST] Initializing IPC server...\n");
    if (ipc_server_init(&conn) != 0)
    {
        fprintf(stderr, "[TEST] Failed to initialize server\n");
        return 1;
    }
    
    // Wait for Python client to connect
    printf("[TEST] Waiting for Python client to connect...\n");
    printf("[TEST] (Run 'python3 test_ipc.py' in another terminal)\n\n");
    
    if (ipc_accept_client(&conn) != 0) {
        fprintf(stderr, "[TEST] Failed to accept client\n");
        ipc_cleanup(&conn);
        return 1;
    }
    
    // Send START command
    printf("\n[TEST] Sending START command...\n");

    // Test values for video metadata
    double test_duration = 30.0;  // 30 seconds
    double test_fps = 30.0;
    int test_loop = 0;            // false
    int test_frame_interval = 5;
    if (ipc_send_start(&conn, video_path, test_duration, test_fps, 
                       test_loop, test_frame_interval) != 0)
    {
        fprintf(stderr, "[TEST] Failed to send START\n");
        ipc_cleanup(&conn);
        return 1;
    }
    
    // Receive messages using poll() for non-blocking operation
    printf("\n[TEST] Waiting for detection messages (Ctrl+C to stop)...\n\n");
    
    struct pollfd pfd;
    pfd.fd = ipc_get_client_fd(&conn);
    pfd.events = POLLIN;
    
    while (running && ipc_check_client_connected(&conn))
    {
        // Poll with 1 second timeout
        int ret = poll(&pfd, 1, 1000);
        
        if (ret > 0 && (pfd.revents & POLLIN))
        {
            // Data available
            int len = ipc_recv_message(&conn, buffer, sizeof(buffer));
            
            if (len > 0)
            {
                msg_count++;
                printf("[TEST] Message #%d: %s\n", msg_count, buffer);
                
                // Stop after FRAMES_REQUIRED messages for demo
                if (msg_count >= FRAMES_REQUIRED)
                {
                    printf("\n[TEST] Received %d messages as required, sending STOP...\n", FRAMES_REQUIRED);
                    break;
                }
            } else if (len < 0) {
                printf("[TEST] Client disconnected or error\n");
                break;
            }
        }
        
        // Update poll fd in case client reconnected
        pfd.fd = ipc_get_client_fd(&conn);
    }
    
    // Send STOP command if client still connected
    if (ipc_check_client_connected(&conn)) {
        printf("\n[TEST] Sending STOP command...\n");
        ipc_send_stop(&conn);
        
        // Wait a bit for client to receive STOP command
        usleep(100000);  // 100ms
    }
    
    // Cleanup
    printf("\n[TEST] Cleaning up...\n");
    ipc_cleanup(&conn);
    printf("Test Complete - Received %d messages\n", msg_count);

    return 0;
}