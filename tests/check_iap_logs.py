import sys
import time
import os
from playwright.sync_api import sync_playwright

def view_logs():
    print("Starting Playwright script for checking IAP logs...", flush=True)
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

        # Click Logs sidebar
        print("Navigating to Logs tab...", flush=True)
        logs_tab = page.locator("a:has-text('Logs')")
        if logs_tab.count() > 0:
            logs_tab.first.click()
        else:
            page.click(".wt-sidebar-item:has-text('Logs')")
        
        page.wait_for_timeout(2000)

        # Select INBOUND_AUDIO_PROCESSOR filter
        print("Filtering by INBOUND_AUDIO_PROCESSOR...", flush=True)
        page.select_option("#logServiceFilter", "INBOUND_AUDIO_PROCESSOR")
        page.wait_for_timeout(5000)
        page.screenshot(path="/tmp/pw_iap_logs.png")

        # Dump results
        logs_text = page.locator("#liveLogView").inner_text() or ""
        print("\n=== IAP LOGS ===")
        print(logs_text, flush=True)
        print("================\n", flush=True)

        browser.close()

if __name__ == "__main__":
    view_logs()
