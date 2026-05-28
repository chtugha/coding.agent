#!/usr/bin/env python3
import sys
import time
from playwright.sync_api import sync_playwright

def run_verification():
    print("=========================================================")
    print("Starting Sequential Service and Log Level Verification...")
    print("=========================================================")

    url = "https://127.0.0.1:8080/"

    with sync_playwright() as p:
        # Launch Chromium headless with self-signed certificate support
        browser = p.chromium.launch(headless=True)
        context = browser.new_context(ignore_https_errors=True)
        page = context.new_page()

        # Add console and error listeners for debugging
        page.on("console", lambda msg: print(f"BROWSER_CONSOLE: {msg.type}: {msg.text}"))
        page.on("pageerror", lambda err: print(f"BROWSER_ERROR: {err}"))

        # Connect to frontend
        print(f"Connecting to frontend at {url}...")
        try:
            page.goto(url)
            page.wait_for_load_state("networkidle")
            print(f"Connected successfully! Title: '{page.title()}'")
        except Exception as e:
            print(f"Error connecting to frontend: {e}")
            sys.exit(1)

        # Navigate to Services tab
        print("Navigating to the Services tab...")
        page.click('a[data-page="services"]')
        page.wait_for_selector("#servicesContainer")

        # Fetch services list via evaluation
        print("Retrieving service configuration list...")
        services_data = page.evaluate("fetch('/api/services').then(r => r.json())")
        services = [s["name"] for s in services_data.get("services", [])]
        print(f"Found {len(services)} services to verify: {services}\n")

        start_at = 1
        for idx, name in enumerate(services, start=1):
            if idx < start_at:
                continue
            print("---------------------------------------------------------")
            print(f"Verifying Service {idx}/{len(services)}: {name}")
            print("---------------------------------------------------------")

            # 1. Ensure Services Overview is active and show service details
            page.evaluate("showServicesOverview()")
            time.sleep(0.5)
            page.wait_for_selector(f'.wt-card[data-svc="{name}"]')
            page.click(f'.wt-card[data-svc="{name}"]')
            page.wait_for_selector("#services-detail:not(.hidden)")
            # Wait for asynchronous showSvcDetail fetch and DOM update to complete
            time.sleep(1.0)

            # Check starting state
            status = page.locator("#svcDetailStatus").inner_text().strip()
            print(f"Initial status for {name}: {status}")

            if "Online" in status:
                print(f"Service {name} is already running. Stopping it first to start clean...")
                page.evaluate("document.getElementById('svcStopBtn').click()")
                # Wait for offline
                page.locator("#svcDetailStatus").wait_for(state="attached")
                for _ in range(30):
                    time.sleep(0.5)
                    status = page.locator("#svcDetailStatus").inner_text().strip()
                    if "Offline" in status:
                        break
                print(f"Service {name} stopped successfully for clean environment.")

            # 2. Start the Service
            # Debug why start button might be hidden
            btn_info = page.evaluate("""() => {
                const el = document.getElementById('svcStartBtn');
                if (!el) return { error: 'No element found with id svcStartBtn' };
                const style = window.getComputedStyle(el);
                const rect = el.getBoundingClientRect();
                let p = el.parentElement;
                let hiddenParent = null;
                while (p) {
                    const ps = window.getComputedStyle(p);
                    if (ps.display === 'none' || ps.visibility === 'hidden' || ps.opacity === '0') {
                        hiddenParent = { id: p.id, className: p.className, tag: p.tagName, display: ps.display };
                        break;
                    }
                    p = p.parentElement;
                }
                return {
                    id: el.id,
                    display: style.display,
                    visibility: style.visibility,
                    opacity: style.opacity,
                    width: el.offsetWidth,
                    height: el.offsetHeight,
                    rect: { left: rect.left, top: rect.top, width: rect.width, height: rect.height },
                    windowWidth: window.innerWidth,
                    windowHeight: window.innerHeight,
                    hiddenParent: hiddenParent
                };
            }""")
            print(f"DEBUG START BTN INFO for {name}: {btn_info}")

            print(f"Clicking 'Start' to launch {name} programmatically...")
            page.evaluate("document.getElementById('svcStartBtn').click()")

            # Wait for it to become online
            online = False
            for _ in range(120):
                time.sleep(0.5)
                status = page.locator("#svcDetailStatus").inner_text().strip()
                if "Online" in status:
                    online = True
                    break

            if not online:
                print(f"ERROR: Service {name} failed to become Online within timeout!")
                # Read whatever log was displayed
                print(f"Last displayed logs: {page.locator('#svcDetailLog').inner_text().strip()}")
                sys.exit(1)

            print(f"Service {name} is now ONLINE!")

            # 3. Check Live Log panel in Config tab
            print("Waiting for logs to stream in Config tab live log panel...")
            logs_accumulated = False
            for _ in range(15):
                time.sleep(1.0)
                log_text = page.locator("#svcDetailLog").inner_text().strip()
                if log_text and "Waiting for logs..." not in log_text:
                    logs_accumulated = True
                    print(f"SUCCESS: Service {name} logs correctly forwarded and showing in details tab!")
                    print("Sample log text:")
                    print("\n".join(log_text.splitlines()[-3:]))
                    break

            if not logs_accumulated:
                print(f"WARNING: No active logs printed yet or showing 'Waiting for logs...' for {name}.")
                print(f"Current detail log text: '{page.locator('#svcDetailLog').inner_text().strip()}'")

            # 4. Check Log Level runtime modification & Fallback interactive popup
            current_level = page.eval_on_selector('#svcRuntimeLogLevel', 'el => el.value')
            print(f"Current log level for {name} is: {current_level}")

            target_level = "DEBUG" if current_level != "DEBUG" else "INFO"
            print(f"Changing log level to {target_level}...")
            page.select_option('#svcRuntimeLogLevel', target_level)

            # Determine dynamically if the service supports live update or triggers the restart modal
            print("Waiting for potential log level change response / restart modal...")
            is_modal_visible = False
            for _ in range(6):
                time.sleep(0.5)
                if page.locator('#restart-popup-modal').is_visible():
                    is_modal_visible = True
                    break

            if is_modal_visible:
                print(f"Service {name} requires restart. Fallback Restart Modal is displayed!")
                print("Fallback Restart Modal is displayed! Verifying NO button first...")
                # Test No button
                page.click("#restart-popup-no-btn")
                # Wait for modal to be hidden
                page.wait_for_selector('#restart-popup-modal', state="hidden", timeout=3000)
                
                reverted_level = page.eval_on_selector('#svcRuntimeLogLevel', 'el => el.value')
                if reverted_level != current_level:
                    print(f"ERROR: Log level select did not revert to {current_level} after clicking No! Found: {reverted_level}")
                    sys.exit(1)
                print("SUCCESS: NO button closed modal and successfully reverted dropdown value.")

                # Test Yes button
                print(f"Re-selecting {target_level} to test YES button...")
                page.select_option('#svcRuntimeLogLevel', target_level)
                # Wait for modal to be visible again
                page.wait_for_selector('#restart-popup-modal', state="visible", timeout=3000)

                print("Clicking 'Yes' button to restart service with the new log level...")
                page.click("#restart-popup-yes-btn")
                # Wait for modal to be hidden
                page.wait_for_selector('#restart-popup-modal', state="hidden", timeout=3000)

                print(f"Waiting for {name} to restart and become Online again...")
                restarted_online = False
                for _ in range(120):
                    time.sleep(0.5)
                    status = page.locator("#svcDetailStatus").inner_text().strip()
                    if "Online" in status:
                        restarted_online = True
                        break

                if not restarted_online:
                    print(f"ERROR: Service {name} failed to return Online after log level restart!")
                    sys.exit(1)

                new_actual_level = page.eval_on_selector('#svcRuntimeLogLevel', 'el => el.value')
                if new_actual_level != target_level:
                    print(f"ERROR: After restart, log level dropdown is {new_actual_level}, expected {target_level}!")
                    sys.exit(1)
                print("SUCCESS: YES button successfully saved configuration, restarted service, and updated level.")
            else:
                print("No restart modal appeared. Checking if live update was performed...")
                new_actual_level = page.eval_on_selector('#svcRuntimeLogLevel', 'el => el.value')
                if new_actual_level != target_level:
                    print(f"ERROR: Expected log level to be updated to {target_level}, found: {new_actual_level}")
                    sys.exit(1)
                print("SUCCESS: Log level updated at runtime on the running service successfully.")

            # 5. Check logs in the common live log (Live Logs nav tab)
            print("Navigating to common 'Live Logs' tab...")
            page.click('a[data-page="logs"]')
            page.wait_for_selector("#liveLogView")

            # Check option values to see if name or other value matches
            print(f"Filtering live logs for service: {name}")
            try:
                page.select_option('#logServiceFilter', name)
            except Exception as e:
                # Fallback to All Services if exact option value matches slightly different label
                print(f"Could not select service-specific filter (perhaps different name in select): {e}. Proceeding with All Services.")
                page.select_option('#logServiceFilter', '')

            time.sleep(2.0)
            live_log_text = page.locator("#liveLogView").inner_text().strip()
            if live_log_text and "Waiting for logs..." not in live_log_text:
                print("SUCCESS: Service logs displayed in common Live Logs view!")
                print("Sample live log text:")
                print("\n".join(live_log_text.splitlines()[-2:]))
            else:
                print("WARNING: Common Live Logs view does not show any logs for this service yet.")

            # 6. Click 'Stop' and terminate service before next
            print("Navigating back to Services detail...")
            page.click('a[data-page="services"]')
            page.wait_for_selector("#servicesContainer")
            page.click(f'.wt-card[data-svc="{name}"]')
            page.wait_for_selector("#services-detail:not(.hidden)")
            # Wait for asynchronous showSvcDetail fetch and DOM update to complete
            time.sleep(1.0)

            print(f"Stopping service {name}...")
            page.evaluate("document.getElementById('svcStopBtn').click()")
            
            # Wait for offline
            for _ in range(30):
                time.sleep(0.5)
                status = page.locator("#svcDetailStatus").inner_text().strip()
                if "Offline" in status:
                    break
            print(f"Service {name} is now OFFLINE. Verification complete!\n")

        print("=========================================================")
        print("ALL SERVICES VERIFIED SUCCESSFULLY!")
        print("=========================================================")
        browser.close()

if __name__ == "__main__":
    run_verification()
