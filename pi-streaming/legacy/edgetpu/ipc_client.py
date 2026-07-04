"""
ipc_client.py

Unix Domain Socket IPC client for Python detection script.

This module connects to the C Unix socket server and:
- Receives START/STOP commands
- Sends detection JSON data

Usage:
    from ipc_client import IPCClient
    
    client = IPCClient()
    client.connect()
    
    # Wait for start command
    cmd = client.wait_for_command()
    if cmd['cmd'] == 'start':
        video_path = cmd['video_path']
        # ... process video ...
        
        # Send detection data
        client.send_detection(timestamp_ms, detections)
    
    client.close()
"""

import socket
import json
import os
import time
from typing import Optional, Dict, Any, List

# Must match SOCKET_PATH in unix_socket.h
SOCKET_PATH = "/tmp/detection.sock"

# Buffer size for receiving
RECV_BUFFER_SIZE = 4096


class IPCClient:
    """
    IPC Client for communicating with the C Unix socket server.
    
    Uses Unix Domain Sockets for fast local communication.
    Messages are newline-delimited JSON.
    """
    
    def __init__(self, socket_path: str = SOCKET_PATH):
        """
        Initialize the IPC client.
        
        Args:
            socket_path: Path to the Unix domain socket (default: /tmp/detection.sock)
        """
        self.socket_path = socket_path
        self.sock: Optional[socket.socket] = None
        self.recv_buffer = ""
        self.connected = False
    
    def connect(self, timeout: float = 30.0, retry_interval: float = 1.0) -> bool:
        """
        Connect to the C server.

        Will retry connection until timeout if server isn't ready yet.

        Args:
            timeout: Maximum time to wait for connection (seconds)
            retry_interval: Time between connection attempts (seconds)

        Returns:
            True if connected, False if timeout
        """
        start_time = time.time()

        while time.time() - start_time < timeout:
            try:
                # Create Unix domain socket
                self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                # Increase send buffer for high-throughput IPC
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024 * 1024)
                self.sock.connect(self.socket_path)
                self.connected = True
                print(f"[IPC] Connected to {self.socket_path}")
                return True
            except FileNotFoundError:
                print(f"[IPC] Socket not found, retrying in {retry_interval}s...")
                if self.sock:
                    self.sock.close()
                    self.sock = None
                time.sleep(retry_interval)
            except ConnectionRefusedError:
                print(f"[IPC] Connection refused, retrying in {retry_interval}s...")
                if self.sock:
                    self.sock.close()
                    self.sock = None
                time.sleep(retry_interval)
            except Exception as e:
                print(f"[IPC] Connection error: {e}")
                if self.sock:
                    self.sock.close()
                    self.sock = None
                time.sleep(retry_interval)
        
        print(f"[IPC] Connection timeout after {timeout}s")
        return False
    
    def wait_for_command(self, timeout: Optional[float] = None) -> Optional[Dict[str, Any]]:
        """
        Wait for a command from the C server.
        
        Blocks until a command is received or timeout.
        
        Args:
            timeout: Maximum time to wait (None = wait forever)
            
        Returns:
            Command dictionary (e.g., {'cmd': 'start', 'video_path': '...'})
            or None if timeout/disconnected
        """
        if not self.connected or not self.sock:
            print("[IPC] Not connected")
            return None
        
        # Set socket timeout
        self.sock.settimeout(timeout)
        
        try:
            while True:
                # Check if we already have a complete message in buffer
                newline_pos = self.recv_buffer.find('\n')
                if newline_pos != -1:
                    # Extract message
                    msg = self.recv_buffer[:newline_pos]
                    self.recv_buffer = self.recv_buffer[newline_pos + 1:]
                    
                    try:
                        cmd = json.loads(msg)
                        print(f"[IPC] Received command: {cmd}")
                        return cmd
                    except json.JSONDecodeError as e:
                        print(f"[IPC] Invalid JSON received: {msg} ({e})")
                        continue
                
                # Need more data
                data = self.sock.recv(RECV_BUFFER_SIZE)
                if not data:
                    # Server closed connection
                    print("[IPC] Server disconnected")
                    self.connected = False
                    return None
                
                self.recv_buffer += data.decode('utf-8')
                
        except socket.timeout:
            return None
        except Exception as e:
            print(f"[IPC] Error receiving command: {e}")
            self.connected = False
            return None
    
    def check_for_command(self) -> Optional[Dict[str, Any]]:
        """
        Non-blocking check for a command.
        
        Returns immediately if no command available.
        
        Returns:
            Command dictionary or None if no command available
        """
        return self.wait_for_command(timeout=0.01)
    
    def send_detection(self, timestamp_ms: int, detections: List[Dict[str, Any]]) -> bool:
        """
        Send detection data to the C server.

        Args:
            timestamp_ms: Video timestamp in milliseconds (PTS)
            detections: List of detection dictionaries, each containing:
                - id: Detection ID (string)
                - class: Object class (e.g., "person")
                - confidence: Confidence score (0.0 to 1.0)
                - bbox: Bounding box dict with x, y, width, height

        Returns:
            True if sent successfully, False otherwise
        """
        if not self.connected or not self.sock:
            print("[IPC] Not connected")
            return False

        msg = {
            "type": "target_detection",
            "timestamp_ms": timestamp_ms,
            "detections": detections
        }

        try:
            # Reset timeout for send (check_for_command sets it to 10ms)
            self.sock.settimeout(5.0)  # 5 second timeout for sends
            # Serialize to JSON with newline delimiter
            data = json.dumps(msg, separators=(',', ':')) + '\n'
            self.sock.sendall(data.encode('utf-8'))
            return True
        except Exception as e:
            print(f"[IPC] Error sending detection: {e}")
            self.connected = False
            return False
    
    def send_raw(self, data: Dict[str, Any]) -> bool:
        """
        Send raw JSON data to the C server.

        Args:
            data: Dictionary to send as JSON

        Returns:
            True if sent successfully, False otherwise
        """
        if not self.connected or not self.sock:
            print("[IPC] Not connected")
            return False

        try:
            # Reset timeout for send (check_for_command sets it to 10ms)
            self.sock.settimeout(5.0)  # 5 second timeout for sends
            msg = json.dumps(data, separators=(',', ':')) + '\n'
            self.sock.sendall(msg.encode('utf-8'))
            return True
        except Exception as e:
            print(f"[IPC] Error sending data: {e}")
            self.connected = False
            return False
    
    def is_connected(self) -> bool:
        """Check if still connected to server."""
        return self.connected and self.sock is not None
    
    def close(self):
        """Close the connection."""
        if self.sock:
            try:
                self.sock.close()
            except:
                pass
            self.sock = None
        self.connected = False
        self.recv_buffer = ""
        print("[IPC] Connection closed")


# Convenience function for creating detection dictionaries
def make_detection(
    detection_id: str,
    obj_class: str,
    confidence: float,
    x: int,
    y: int,
    width: int,
    height: int
) -> Dict[str, Any]:
    """
    Create a detection dictionary in the expected format.
    
    Args:
        detection_id: Unique ID for this detection (e.g., "1", "2")
        obj_class: Object class (e.g., "person", "car")
        confidence: Detection confidence (0.0 to 1.0)
        x: Bounding box left coordinate (pixels)
        y: Bounding box top coordinate (pixels)
        width: Bounding box width (pixels)
        height: Bounding box height (pixels)
        
    Returns:
        Detection dictionary ready for send_detection()
    """
    return {
        "id": detection_id,
        "class": obj_class,
        "confidence": round(confidence, 3),
        "bbox": {
            "x": x,
            "y": y,
            "width": width,
            "height": height
        }
    }