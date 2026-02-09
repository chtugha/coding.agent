import socket
import time

def test_llama(cid, text):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(("127.0.0.1", 8083))
    msg = f"{cid}:{text}"
    start = time.time()
    sock.send(msg.encode())
    sock.close()
    print(f"Sent: {text}")

if __name__ == "__main__":
    # Note: llama-service needs to be running for this to work
    # We just test the sending part here or we could mock the receiver
    test_llama(1, "Hallo, wer bist du?")
    time.sleep(1)
    test_llama(1, "Wie ist das Wetter in Berlin?")
