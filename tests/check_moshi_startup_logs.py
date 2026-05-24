import sys
import time
import os
from playwright.sync_api import sync_playwright

def view_startup_logs():
    print("Starting Playwright script for checking Moshi startup logs...", flush=True)
    with sync_playwright() as p:
        browser = p.chromium.launch(headless=True, args=["--ignore-certificate-errors"])
        context = browser.new_context(ignore_https_errors=True)
        page = context.new_page()

        print("Navigating to https://localhost:8080/...", flush=True)
        try:
            page.goto("https://localhost:8080/", timeout=15000)
        except Exception as e:
            print(f"Failed to navigate via HTTPS, trying plain HTTP: {e}", flush=True)
            page.goto("http://localhost:8081/", timeout=15000)

        page.wait_for_timeout(2000)

        # Check if login is required
        if page.locator("input[name='username']").count() > 0:
            print("Login page detected. Logging in...", flush=True)
            page.fill("input[name='username']", "admin")
            page.fill("input[name='password']", "admin")
            page.click("button[type='submit']")
            page.wait_for_timeout(3000)

        # Click Database sidebar
        print("Navigating to Database tab...", flush=True)
        db_tab = page.locator("a:has-text('Database')")
        if db_tab.count() > 0:
            db_tab.first.click()
        else:
            page.click(".wt-sidebar-item:has-text('Database')")
        
        page.wait_for_timeout(2000)

        # Execute query
        print("Running SQL query...", flush=True)
        query = "SELECT timestamp, level, message FROM logs WHERE service='MOSHI_SERVICE' ORDER BY id DESC LIMIT 100"
        page.fill("#sqlQuery", query)
        page.click("button:has-text('Execute')")
        
        page.wait_for_timeout(4000)
        page.screenshot(path="/tmp/pw_db_query.png")

        # Get results
        results_text = page.locator("#queryResults").inner_text() or ""
        print("\n=== SQL QUERY RESULTS ===")
        print(results_text, flush=True)
        print("=========================\n", flush=True)

        browser.close()

if __name__ == "__main__":
    view_startup_logs()
