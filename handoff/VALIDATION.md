# Web workspace validation

- Built native C++ Agent with Clang 19 / C++17, warnings enabled.
- All 84 C++ self-tests passed (`web-selftest.log`).
- All 41 Python tests passed, including HTTP authentication, origin/CSRF, session revocation, private configuration, safe file reads, retry deduplication, and personal event/approval boundaries.
- Real Chromium + native C++ Agent + local fake Responses provider: login, file browsing, approval/write, session continuation, option-shaped prompt, provider configuration, mobile overflow, logout passed. Agent Bubblewrap was enabled; this test ran outside the outer command sandbox to allow native namespace setup.
- Native C++ collaboration regression: FIFO queue, real tool writes, cross-member approval, session resume passed with Bubblewrap enabled.
- Disposable Linux container + actual member and gateway UIDs + SO_PEERCRED + HTTP + Git passed. Tests cover personal credential separation, private jobs/events/approvals, password rotation revocation, unauthorized relay forgery, conflict abort/resolution, stale confirmation, and member removal.
- Chromium over a test HTTPS reverse proxy passed shared conversation, cross-member approval, Secure/HttpOnly/SameSite cookies, credential roles, checkpoint, exact-SHA merge confirmation/cancel, and personal file browsing (`web-collaboration-integration.log`).
- Container Agent uses the explicit protocol fixture because this environment cannot mount nested procfs. This does not establish native Agent isolation inside Docker. Native Agent validation is separate, above.
- `node --check web/app.js`, Python compilation, shell syntax, and `git diff --check` passed.
- Screenshots: web-desktop.png, web-mobile.png, web-server-desktop.png.
- No real provider credentials or paid model calls used. No user server deployment. Test containers removed.
- Linux tested. macOS/Windows hosting not validated; Windows hosting is explicitly unavailable. Python standard-library runtime and companion server/web assets are required.
