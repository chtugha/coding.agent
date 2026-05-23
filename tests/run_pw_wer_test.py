import sys
import time
import os
from playwright.sync_api import sync_playwright

def run_wer_test():
    print("Starting Playwright script for Moshi RAG WER test...")
    with sync_playwright() as p:
        # Launch Chromium with self-signed certificate errors ignored
        browser = p.chromium.launch(headless=True, args=["--ignore-certificate-errors"])
        context = browser.new_context(ignore_https_errors=True)
        page = context.new_page()

        # 1. Navigate to the frontend login page
        print("Navigating to https://127.0.0.1:8080/...")
        try:
            page.goto("https://127.0.0.1:8080/", timeout=15000)
        except Exception as e:
            print(f"Failed to navigate via HTTPS, trying plain HTTP: {e}")
            page.goto("http://127.0.0.1:8081/", timeout=15000)

        page.wait_for_timeout(2000)
        page.screenshot(path="/tmp/pw_wer_login.png")
        print("Screenshot saved to /tmp/pw_wer_login.png")

        # Check if login is required
        if page.locator("input[name='username']").count() > 0:
            print("Login page detected. Logging in...")
            page.fill("input[name='username']", "admin")
            page.fill("input[name='password']", "admin")
            page.click("button[type='submit']")
            page.wait_for_timeout(3000)
            page.screenshot(path="/tmp/pw_wer_dashboard.png")
            print("Dashboard screenshot saved to /tmp/pw_wer_dashboard.png")

        # 2. Navigate to Tests tab
        print("Navigating to Tests tab...")
        tests_tab = page.locator("a:has-text('Tests')")
        if tests_tab.count() > 0:
            tests_tab.first.click()
        else:
            page.click(".wt-sidebar-item:has-text('Tests')")
        
        page.wait_for_timeout(3000)

        # 3. Click "PIPELINE TESTS" sub-tab
        print("Clicking PIPELINE TESTS sub-tab...")
        page.click("text=PIPELINE TESTS")
        page.wait_for_timeout(2000)
        page.screenshot(path="/tmp/pw_wer_pipeline_tests_tab.png")
        print("Pipeline tests sub-tab screenshot saved to /tmp/pw_wer_pipeline_tests_tab.png")

        # 4. Find and expand "TEST 3: FULL LOOP FILE TEST (WER)"
        # Let's see if we need to click the card to expand it. Cards are typically expandable.
        card_header = page.locator("text=TEST 3: FULL LOOP FILE TEST (WER)")
        if card_header.count() > 0:
            print("Expanding TEST 3 card...")
            card_header.first.click()
            page.wait_for_timeout(1000)
            page.screenshot(path="/tmp/pw_wer_test3_expanded.png")

        # 5. Select Moshi RAG Engine in the Full Loop Test Card
        print("Selecting Moshi RAG engine...")
        page.select_option("#fullLoopEngine", "moshi-rag")
        page.wait_for_timeout(1000)

        # 6. Select English test file (STT model is en/fr, German files produce 0 transcription)
        print("Selecting sample_en_01.wav...")
        page.select_option("#fullLoopFiles", "sample_en_01.wav")
        page.wait_for_timeout(1000)
        page.screenshot(path="/tmp/pw_wer_tests_configured.png")

        # 7. Click the Run button
        print("Clicking 'Run WER Test' (#fullLoopBtn)...")
        page.click("#fullLoopBtn")
        
        # 8. Monitor progress (moshi model is slow on M4, allow up to 75 minutes)
        print("Monitoring WER test execution...")
        start_time = time.time()
        timeout = 4500
        last_status = ""
        while time.time() - start_time < timeout:
            page.wait_for_timeout(15000)
            status_text = page.locator("#fullLoopStatus").text_content() or ""
            status_text = status_text.strip()
            if status_text != last_status:
                print(f"[{int(time.time() - start_time)}s] Status: {status_text}")
                last_status = status_text
                page.screenshot(path="/tmp/pw_wer_running_progress.png")
            
            results_html = page.locator("#fullLoopResults").inner_html() or ""
            if "Sim%" in results_html or "Avg Sim" in results_html or "avg_similarity" in results_html or "similarity" in results_html.lower():
                print("WER test completed! Results table detected.")
                break
                
            if "Error" in status_text or "Failed" in status_text:
                print(f"Error detected in status: {status_text}")
                break

        page.wait_for_timeout(3000)
        page.screenshot(path="/tmp/pw_wer_completed.png")
        print("Final screenshot saved to /tmp/pw_wer_completed.png")

        results_text = page.locator("#fullLoopResults").inner_text() or ""
        print("\n=== WER TEST RESULTS ===")
        print(results_text)
        print("========================\n")

        print("Checking MOSHI_SERVICE logs via UI Logs tab...")
        logs_tab = page.locator("a:has-text('Logs')")
        if logs_tab.count() > 0:
            logs_tab.first.click()
            page.wait_for_timeout(2000)
            page.screenshot(path="/tmp/pw_wer_logs.png")
            print("Logs tab screenshot saved to /tmp/pw_wer_logs.png")

        browser.close()

if __name__ == "__main__":
    run_wer_test()
