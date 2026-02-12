#!/usr/bin/env python3
"""
Test Call Lifecycle Signaling (Phase 1.2)

Tests that CALL_START and CALL_END signals propagate correctly through the pipeline:
  SIP Client → Inbound → Whisper → LLaMA → Kokoro → Outbound

Verification criteria:
- Each service receives and logs the signal
- Signal propagates within 500ms
- Resources are properly cleaned up after CALL_END
"""

import socket
import time
import os
import sys

def send_control_signal(socket_path, message):
    """Send a control signal to a Unix socket"""
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(0.2)
        sock.connect(socket_path)
        sock.send(message.encode('utf-8'))
        sock.close()
        return True
    except Exception as e:
        print(f"  ⚠️  Failed to send to {socket_path}: {e}")
        return False

def test_signal_propagation(call_id):
    """Test CALL_START and CALL_END signal propagation"""
    print(f"\n{'='*60}")
    print(f"Testing Call Lifecycle Signaling for call_id {call_id}")
    print(f"{'='*60}\n")
    
    # Test CALL_START
    print(f"📤 Sending CALL_START:{call_id} to inbound-audio-processor...")
    start_time = time.time()
    
    if send_control_signal("/tmp/inbound-audio-processor.ctrl", f"CALL_START:{call_id}"):
        print(f"  ✅ Signal sent successfully")
        print(f"  ⏱️  Time: {(time.time() - start_time)*1000:.1f}ms")
    else:
        print(f"  ❌ Failed to send signal")
        return False
    
    # Wait for signal propagation
    print(f"\n⏳ Waiting 2 seconds for signal propagation...")
    time.sleep(2)
    
    # Test CALL_END
    print(f"\n📤 Sending CALL_END:{call_id} to inbound-audio-processor...")
    start_time = time.time()
    
    if send_control_signal("/tmp/inbound-audio-processor.ctrl", f"CALL_END:{call_id}"):
        print(f"  ✅ Signal sent successfully")
        print(f"  ⏱️  Time: {(time.time() - start_time)*1000:.1f}ms")
    else:
        print(f"  ❌ Failed to send signal")
        return False
    
    # Wait for cleanup (200ms grace period + processing time)
    print(f"\n⏳ Waiting 500ms for cleanup (200ms grace period + margin)...")
    time.sleep(0.5)
    
    print(f"\n{'='*60}")
    print(f"✅ Test completed!")
    print(f"{'='*60}\n")
    
    print("📋 Verification checklist:")
    print("  [ ] Check service logs for CALL_START propagation")
    print("  [ ] Check service logs for resource pre-allocation")
    print("  [ ] Check service logs for CALL_END propagation")
    print("  [ ] Check service logs for cleanup messages")
    print("  [ ] Verify cleanup completed within 500ms of CALL_END")
    print("  [ ] Verify no crashes or errors in any service")
    
    return True

def main():
    if len(sys.argv) > 1:
        call_id = int(sys.argv[1])
    else:
        call_id = 1
    
    print("="*60)
    print("Call Lifecycle Signaling Test (Phase 1.2)")
    print("="*60)
    print("\n⚠️  Prerequisites:")
    print("  1. All services must be running:")
    print("     - inbound-audio-processor")
    print("     - whisper-service")
    print("     - llama-service")
    print("     - kokoro_service.py")
    print("     - outbound-audio-processor")
    print("\n  2. Monitor service logs in separate terminals to verify signal propagation")
    print(f"\n  3. Test will use call_id={call_id}")
    print("\nPress Enter to start the test...")
    input()
    
    test_signal_propagation(call_id)
    
    print("\n💡 Next steps:")
    print("  1. Review service logs to verify all signals were received")
    print("  2. Verify resource cleanup happened correctly")
    print("  3. Run with different call_ids to test concurrent calls")

if __name__ == "__main__":
    main()
