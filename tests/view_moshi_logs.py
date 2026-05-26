import urllib.request
import json
import ssl

def fetch_logs():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    
    print("Fetching early logs from /api/logs...")
    try:
        # We can pass offset/limit to get the oldest logs first
        url = "https://127.0.0.1:8080/api/logs?service=MOSHI_SERVICE&limit=200&offset=0"
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, context=ctx) as response:
            data = json.loads(response.read().decode())
            logs = data.get("logs", [])
            print(f"Retrieved {len(logs)} log lines (oldest first):")
            for l in logs:
                print(f"{l.get('timestamp')} [{l.get('level')}] {l.get('message')}")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    fetch_logs()
