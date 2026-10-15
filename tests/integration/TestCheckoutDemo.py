"""Check demo setup and listener behavior without Stripe credentials or network calls."""

import concurrent.futures
import hashlib
import hmac
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import urllib.parse


def request(port, method, path, body="", headers=None, timeout=2):
    """Send one local request without following redirects."""
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        connection.request(method, path, body, headers or {})
        response = connection.getresponse()
        return response.status, response.getheader("Location"), response.read().decode()
    finally:
        connection.close()


def save_setup(port, **values):
    """Submit setup values to the demo."""
    return request(port, "POST", "/setup", urllib.parse.urlencode(values),
                   {"Content-Type": "application/x-www-form-urlencoded"})


def follow_home(port, response, notice):
    """Check that a form redirects to the home page and displays its notice."""
    status, location, _ = response
    assert status == 303, response
    assert urllib.parse.urlsplit(location).path == "/", location
    status, _, page = request(port, "GET", location)
    assert status == 200 and notice in page, page
    return page


def wait_for_file(path, server):
    """Wait briefly for the stub to report progress, checking the server is alive."""
    deadline = time.monotonic() + 5
    while not path.exists():
        assert server.poll() is None, "Demo exited unexpectedly"
        if time.monotonic() >= deadline:
            raise TimeoutError(f"Timed out waiting for {path.name}")
        time.sleep(0.02)


def main():
    """Exercise setup, delayed listener startup, and credential preservation."""
    executable = Path(sys.argv[1]).resolve()
    with socket.socket() as reservation:
        reservation.bind(("127.0.0.1", 0))
        port = reservation.getsockname()[1]

    with tempfile.TemporaryDirectory(prefix="stripekit-checkout-test-") as directory:
        root = Path(directory)
        stub = root / "stripe"
        stub.write_text(
            f"#!{sys.executable}\n"
            "import pathlib, sys, time\n"
            "root = pathlib.Path(__file__).parent\n"
            f"assert sys.argv[1:] == ['listen', '--all-snapshot', '--latest', "
            f"'--forward-to', 'http://127.0.0.1:{port}/webhook'], sys.argv\n"
            "with (root / 'Calls').open('a') as calls:\n"
            "    calls.write('listen\\n')\n"
            "(root / 'Started').touch()\n"
            "deadline = time.monotonic() + 10\n"
            "while not (root / 'Release').exists():\n"
            "    if time.monotonic() >= deadline:\n"
            "        sys.exit('Test did not release listener')\n"
            "    time.sleep(0.02)\n"
            "print('Ready! Your webhook signing secret is whsec_test_listener', flush=True)\n"
            "(root / 'Finished').touch()\n"
        )
        stub.chmod(0o700)
        environment = os.environ.copy()
        for name in ("STRIPE_SECRET_KEY", "STRIPE_API_KEY", "STRIPE_WEBHOOK_SECRET",
                     "STRIPE_PRICE_ID"):
            environment.pop(name, None)
        environment["PATH"] = directory + os.pathsep + environment["PATH"]
        environment["STRIPEKIT_DEMO_PORT"] = str(port)

        with (root / "Demo.log").open("w+") as log:
            server = subprocess.Popen([str(executable)], cwd=root, env=environment,
                                      stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 5
                while True:
                    assert server.poll() is None, "Demo exited at startup"
                    try:
                        status, _, home = request(port, "GET", "/")
                        assert status == 200
                        assert 'href="https://docs.stripe.com/testing"' in home
                        assert "4000 0027 6000 3184" in home
                        assert "<li>Success: <code>4242 4242 4242 4242</code></li>" in home
                        assert "<li>Decline: <code>4000 0000 0000 0002</code></li>" in home
                        assert "<li>Always authenticate (3D Secure): <code>4000 0027 6000 3184</code></li>" in home
                        break
                    except ConnectionRefusedError:
                        if time.monotonic() >= deadline:
                            raise TimeoutError("Demo did not start")
                        time.sleep(0.02)

                # Hold the stub until requests on the same event loop have completed.
                with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
                    startup = executor.submit(request, port, "POST", "/stripe-cli/start",
                                              timeout=8)
                    try:
                        wait_for_file(root / "Started", server)
                        assert not startup.done(), "Listener did not wait for its secret"
                        status, _, page = request(port, "GET", "/")
                        assert status == 200 and "Waiting for the Stripe CLI" in page
                        follow_home(port, request(port, "POST", "/stripe-cli/start"),
                                    "already in progress")
                        page = follow_home(
                            port, save_setup(port, api_key="sk_test_demo",
                                             product_name="Saved membership", unit_amount="1500"),
                            "Setup saved.")
                        assert "Saved membership" in page
                        assert 'value="sk_test_' not in page

                        for amount in ("0", "-1", "bad", "12x", "9999999999999999999999"):
                            status, _, page = save_setup(
                                port, product_name="Must not be saved", unit_amount=amount)
                            assert status == 400 and "positive integer" in page
                            assert "Saved membership" in page
                            assert "Must not be saved" not in page
                        for key in ("sk_live_rejected", "invalid"):
                            assert save_setup(port, api_key=key)[0] == 400
                    finally:
                        (root / "Release").touch()
                    page = follow_home(port, startup.result(),
                                       "listener started and webhook secret captured.")
                    assert 'value="whsec_' not in page
                wait_for_file(root / "Finished", server)
                assert (root / "Calls").read_text() == "listen\n"

                # Blank credential fields must preserve the listener's signing secret.
                page = follow_home(port, save_setup(port, api_key="", webhook_secret=""),
                                   "Setup saved.")
                assert "Secret key and webhook signing secret are configured." in page
                payload = json.dumps({"id": "evt_demo_test", "type": "invoice.paid",
                                      "data": {"object": {}}, "livemode": False})
                timestamp = str(int(time.time()))
                signature = hmac.new(b"whsec_test_listener",
                                     f"{timestamp}.{payload}".encode(),
                                     hashlib.sha256).hexdigest()
                status, _, _ = request(
                    port, "POST", "/webhook", payload,
                    {"Content-Type": "application/json",
                     "Stripe-Signature": f"t={timestamp},v1={signature}"})
                assert status == 200, status
                assert "evt_demo_test" in request(port, "GET", "/")[2]
                print("PASS: responsive setup, atomic validation, redirects, concurrent listener "
                      "startup, and saved credentials")
            finally:
                (root / "Release").touch()
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
                if sys.exc_info()[0] is not None:
                    log.seek(0)
                    print(log.read(), file=sys.stderr)


if __name__ == "__main__":
    main()
