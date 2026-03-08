#!/usr/bin/env python3
"""
Reproduce and validate the blocking buzzer crash bug.

Sends rapid test detections to a Flock-You device running test firmware
(xiao_esp32s3_test). Detects crash by monitoring for connection failure
followed by detection count reset.

Usage:
    python3 test/test_buzzer_crash.py [--host 192.168.4.1] [--fixed]

    --fixed   Run against firmware with the deferred buzzer fix applied.
              Expects NO crash after sustained injections.
"""

import argparse
import sys
import time
import urllib.request
import urllib.error
import json


DEVICE_TYPES = ["flock", "raven", "soundthinking", "mfr"]


def fetch(url, timeout=5):
    """Fetch URL, return (response_text, True) or (error_msg, False)."""
    try:
        req = urllib.request.urlopen(url, timeout=timeout)
        return req.read().decode("utf-8", errors="replace"), True
    except urllib.error.URLError as e:
        return str(e), False
    except Exception as e:
        return str(e), False


def get_detection_count(host):
    """Return current detection count, or -1 on connection failure."""
    body, ok = fetch(f"http://{host}/api/detections")
    if not ok:
        return -1
    try:
        return len(json.loads(body))
    except json.JSONDecodeError:
        return -1


def wait_for_device(host, timeout=30):
    """Wait for the device to come back online. Returns True if it did."""
    print(f"  Waiting for device to come back (up to {timeout}s)...", end="", flush=True)
    start = time.time()
    while time.time() - start < timeout:
        count = get_detection_count(host)
        if count >= 0:
            print(f" online (detections: {count})")
            return True
        time.sleep(1)
        print(".", end="", flush=True)
    print(" timeout")
    return False


def clear_detections(host):
    """Clear all detections on the device."""
    _, ok = fetch(f"http://{host}/api/clear")
    return ok


def inject_detection(host, device_type="flock"):
    """Inject a test detection. Returns (response, success)."""
    return fetch(f"http://{host}/api/test/inject?type={device_type}")


def run_crash_test(host, expect_crash):
    """
    Send rapid detections and check for crash/reboot.
    Returns True if the test result matches expectations.
    """
    mode = "UNFIXED (expecting crash)" if expect_crash else "FIXED (expecting stability)"
    print(f"\n{'='*60}")
    print(f"  Buzzer crash test — {mode}")
    print(f"  Target: {host}")
    print(f"{'='*60}\n")

    # Verify device is reachable
    count = get_detection_count(host)
    if count < 0:
        print("ERROR: Device not reachable. Is it powered on and running test firmware?")
        return False

    # Clear any existing detections
    print("Clearing detections...")
    clear_detections(host)
    time.sleep(1)

    # Wait for fyTriggered to reset (needs 30s without detections)
    # But if we just cleared, we need the first injection to trigger the buzzer
    print("Waiting 5s for trigger state to settle...")
    time.sleep(5)

    # Send rapid detections
    max_injections = 30
    crash_detected = False
    successful = 0
    failed = 0
    garbage_seen = False

    print(f"Injecting up to {max_injections} detections (interval: 3s)...\n")

    for i in range(max_injections):
        device_type = DEVICE_TYPES[i % len(DEVICE_TYPES)]
        body, ok = inject_detection(host, device_type)

        if not ok:
            print(f"  #{i+1:2d} [{device_type:14s}] FAILED — connection lost")
            failed += 1
            crash_detected = True
            break

        # Check for garbage bytes in response (corruption precursor)
        try:
            json.loads(body)
            status = "OK"
        except json.JSONDecodeError:
            status = "GARBAGE RESPONSE"
            garbage_seen = True

        print(f"  #{i+1:2d} [{device_type:14s}] {status}")
        successful += 1
        time.sleep(3)

    print(f"\nInjections: {successful} succeeded, {failed} failed")
    if garbage_seen:
        print("WARNING: Garbage bytes detected in responses (memory corruption)")

    # Check if device crashed
    if crash_detected:
        print("\nDevice connection lost — likely crash/reboot.")
        came_back = wait_for_device(host)
        if came_back:
            post_count = get_detection_count(host)
            if post_count == 0:
                print("CONFIRMED: Device rebooted — detection count reset to 0.")
                print("BUG REPRODUCED: Blocking delay() in callback caused watchdog reset.\n")
            elif post_count > 0:
                print(f"Device back with {post_count} detections (SPIFFS may have saved partial data).\n")
        else:
            print("Device did not come back within timeout.\n")
    else:
        # No crash — verify detections are intact
        post_count = get_detection_count(host)
        print(f"\nNo crash detected. Final detection count: {post_count}")

    # Evaluate result
    if expect_crash:
        if crash_detected:
            print("PASS: Crash occurred as expected on unfixed firmware.")
            return True
        else:
            print("INCONCLUSIVE: No crash observed. Device may need more sustained load,")
            print("or the bug may require real BLE callback context to trigger.")
            return False
    else:
        if crash_detected:
            print("FAIL: Crash occurred on supposedly fixed firmware!")
            return False
        else:
            print("PASS: No crash — deferred buzzer fix is working.")
            return True


def main():
    parser = argparse.ArgumentParser(description="Test for blocking buzzer crash bug")
    parser.add_argument("--host", default="192.168.4.1", help="Device IP (default: 192.168.4.1)")
    parser.add_argument("--fixed", action="store_true", help="Expect no crash (testing fixed firmware)")
    args = parser.parse_args()

    expect_crash = not args.fixed
    passed = run_crash_test(args.host, expect_crash)
    sys.exit(0 if passed else 1)


if __name__ == "__main__":
    main()
