from playwright.sync_api import sync_playwright
import time

def query_db():
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True, args=["--ignore-certificate-errors"])
        context = browser.new_context(ignore_https_errors=True)
        page = context.new_page()
        
        print("Navigating to https://127.0.0.1:8080/...")
        try:
            page.goto("https://127.0.0.1:8080/", timeout=15000)
        except Exception:
            page.goto("http://127.0.0.1:8081/", timeout=15000)
            
        page.wait_for_timeout(2000)
        
        if page.locator("input[name='username']").count() > 0:
            page.fill("input[name='username']", "admin")
            page.fill("input[name='password']", "admin")
            page.click("button[type='submit']")
            page.wait_for_timeout(2000)
            
        print("Navigating to Database tab...")
        page.click("a:has-text('Database')")
        page.wait_for_timeout(2000)
        
        # Today is 2026-05-26
        query = "SELECT timestamp, message FROM logs WHERE service='MOSHI_SERVICE' ORDER BY timestamp ASC LIMIT 100"
        print(f"Executing query: {query}")
        page.fill("#sqlQuery", query)
        page.click("text=Execute")
        page.wait_for_timeout(3000)
        
        page.screenshot(path="/tmp/db_query_results_today.png")
        print("Screenshot saved to /tmp/db_query_results_today.png")
        
        # Get the text content of the results
        results_text = page.locator("#queryResults").inner_text() or ""
        print("\n=== QUERY RESULTS (TODAY) ===")
        print(results_text)
        print("=============================\n")
        
        browser.close()

if __name__ == "__main__":
    query_db()
