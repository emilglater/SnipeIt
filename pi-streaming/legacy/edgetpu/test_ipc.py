#!/usr/bin/env python3
"""
test_ipc.py

Test script for Unix Domain Socket IPC

This simulates the Python detection script:
1. Connects to the C server
2. Waits for START command (includes video metadata)
3. Processes frames for the full video duration
4. Supports looping if configured
5. Handles STOP command

Run:
    First start the C server: ./streaming_server
    Then run this: python3 test_ipc.py
"""

import sys
import time
import random

from ipc_client import IPCClient, make_detection


def simulate_detection(frame_num: int):
    """
    Simulate object detection on a frame.
    
    Returns a list of detections (sometimes empty, sometimes multiple).
    """
    detections = []
    
    # Randomly decide how many detections (0-2)
    num_detections = random.choices([0, 1, 2], weights=[0.3, 0.5, 0.2])[0]
    
    for i in range(num_detections):
        # Generate random bounding box
        x = random.randint(50, 500)
        y = random.randint(50, 300)
        width = random.randint(50, 200)
        height = random.randint(100, 300)
        confidence = random.uniform(0.6, 0.99)
        
        detection = make_detection(
            detection_id=str(i + 1),
            obj_class="person",
            confidence=confidence,
            x=x,
            y=y,
            width=width,
            height=height
        )
        detections.append(detection)
    
    return detections


def process_video(client: IPCClient, video_path: str, duration_sec: float, 
                  fps: float, loop: bool, frame_interval: int):
    """
    Process video frames and send detections.
    
    Args:
        client: Connected IPC client
        video_path: Path to video file (for logging)
        duration_sec: Video duration in seconds
        fps: Video frames per second
        loop: Whether to loop the video
        frame_interval: Process every Nth frame
    """
    total_frames = int(duration_sec * fps)
    frame_time_sec = 1.0 / fps  # Time per frame in seconds
    loop_count = 0
    
    print(f"[PROCESS] Video: {video_path}")
    print(f"[PROCESS] Duration: {duration_sec:.2f}s, FPS: {fps:.2f}, Total frames: {total_frames}")
    print(f"[PROCESS] Processing every {frame_interval} frame(s), Loop: {loop}")
    print()
    
    frame_num = 0
    running = True
    
    while running and client.is_connected():
        # Check for STOP command (non-blocking)
        cmd = client.check_for_command()
        if cmd and cmd.get('cmd') == 'stop':
            print("\n[PROCESS] Received STOP command")
            running = False
            break
        
        # Calculate position in current video loop
        video_frame = frame_num % total_frames if total_frames > 0 else frame_num
        
        # Check if we completed a loop
        if total_frames > 0 and frame_num > 0 and video_frame == 0:
            loop_count += 1
            if loop:
                print(f"\n[PROCESS] === Starting loop {loop_count + 1} ===\n")
            else:
                print(f"\n[PROCESS] Video ended after {frame_num} frames")
                running = False
                break
        
        # Process only every Nth frame
        if frame_num % frame_interval == 0:
            # Calculate timestamp based on position in current loop
            timestamp_ms = int(video_frame * frame_time_sec * 1000)
            
            # Get simulated detections
            detections = simulate_detection(frame_num)
            
            # Send detection data
            detection_count = len(detections)
            if detection_count > 0:
                print(f"[PROCESS] Frame {frame_num} (video_frame={video_frame}, ts={timestamp_ms}ms): "
                      f"{detection_count} detection(s)")
            else:
                print(f"[PROCESS] Frame {frame_num} (video_frame={video_frame}, ts={timestamp_ms}ms): "
                      f"No detections")
            
            # Always send the message (even with empty detections)
            if not client.send_detection(timestamp_ms, detections):
                print("[PROCESS] Failed to send detection, stopping")
                running = False
                break
        
        frame_num += 1
        
        # Simulate real-time processing (sleep for frame duration)
        # Using frame_interval to speed up simulation
        time.sleep(frame_time_sec * frame_interval * 0.1)  # 10x faster for demo
    
    return frame_num


def main():
    print("==========================================")
    print("  IPC Test - Python Detection Client")
    print("==========================================\n")
    
    # Create IPC client
    client = IPCClient()
    
    # Connect to C server
    print("[MAIN] Connecting to C server...")
    if not client.connect(timeout=30.0):
        print("[MAIN] Failed to connect to server")
        print("[MAIN] Make sure './streaming_server' is running first")
        sys.exit(1)
    
    print("[MAIN] Connected! Waiting for START command...\n")
    
    total_frames_processed = 0
    session_count = 0
    
    # Main loop - handle multiple streaming sessions
    while client.is_connected():
        # Wait for START command (blocking)
        cmd = client.wait_for_command(timeout=None)
        
        if not cmd:
            print("[MAIN] Connection lost while waiting for command")
            break
        
        if cmd.get('cmd') == 'start':
            session_count += 1
            print(f"\n[MAIN] ========== Session {session_count} ==========")
            print(f"[MAIN] Received START command")
            
            # Extract parameters from START command
            video_path = cmd.get('video_path', 'unknown')
            duration_sec = cmd.get('duration_sec', 30.0)
            fps = cmd.get('fps', 30.0)
            loop = cmd.get('loop', False)
            frame_interval = cmd.get('frame_interval', 5)
            
            # Process the video
            frames = process_video(client, video_path, duration_sec, 
                                   fps, loop, frame_interval)
            total_frames_processed += frames
            
            print(f"\n[MAIN] Session {session_count} ended, processed {frames} frames")
            print("[MAIN] Waiting for next START command...\n")
            
        elif cmd.get('cmd') == 'stop':
            print("[MAIN] Received STOP command (no active session)")
            
        else:
            print(f"[MAIN] Unknown command: {cmd}")
    
    # Cleanup
    print("\n[MAIN] Closing connection...")
    client.close()
    
    print("\n==========================================")
    print(f"  Test Complete")
    print(f"  Sessions: {session_count}")
    print(f"  Total frames processed: {total_frames_processed}")
    print("==========================================")


if __name__ == "__main__":
    main()