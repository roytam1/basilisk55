/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

// Tests that network log messages bring up the network panel.

"use strict";

const TEST_NETWORK_REQUEST_URI =
  "http://example.com/browser/devtools/client/webconsole/test/" +
  "test-network-request.html";

add_task(function* () {
  let finishedRequest = waitForFinishedRequest(({ request }) => {
    return request.url.endsWith("test-network-request.html");
  });

  const hud = yield loadPageAndGetHud(TEST_NETWORK_REQUEST_URI);
  let request = yield finishedRequest;

  yield hud.ui.openNetworkPanel(request.actor);
  let toolbox = gDevTools.getToolbox(hud.target);
  is(toolbox.currentToolId, "netmonitor", "Network panel was opened");
  let panel = toolbox.getCurrentPanel();
  let selected = panel.panelWin.NetMonitorView.RequestsMenu.selectedItem;
  is(selected.method, request.request.method,
     "The correct request is selected");
  is(selected.url, request.request.url,
     "The correct request is definitely selected");
});
