/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const { DOM, PropTypes } = require("devtools/client/shared/vendor/react");
const { connect } = require("devtools/client/shared/vendor/react-redux");
const { PluralForm } = require("devtools/shared/plural-form");
const { L10N } = require("../l10n");
const {
  getDisplayedRequestsSummary,
  getDisplayedTimingMarker
} = require("../selectors/index");
const Actions = require("../actions/index");
const {
  getSizeWithDecimals,
  getTimeWithDecimals,
} = require("../utils/format-utils");

const { button, span } = DOM;

function SummaryButton({
  summary,
  triggerSummary,
  timingMarkers
}) {
  let { count, contentSize, transferredSize, millis } = summary;
  let {
    DOMContentLoaded,
    load,
  } = timingMarkers;
  const text = (count === 0) ? L10N.getStr("networkMenu.empty") :
    PluralForm.get(count, L10N.getStr("networkMenu.summary2"))
    .replace("#1", count)
    .replace("#2", getSizeWithDecimals(contentSize / 1024))
    .replace("#3", getSizeWithDecimals(transferredSize / 1024))
    .replace("#4", getTimeWithDecimals(millis / 1000))
    + ((DOMContentLoaded > -1)
        ? ", " + "DOMContentLoaded: " + L10N.getFormatStrWithNumbers("networkMenu.timeS", getTimeWithDecimals(DOMContentLoaded / 1000))
        : "")
    + ((load > -1)
        ? ", " + "load: " + L10N.getFormatStrWithNumbers("networkMenu.timeS", getTimeWithDecimals(load / 1000))
        : "");

  return button({
    id: "requests-menu-network-summary-button",
    className: "devtools-button",
    title: count ? text : L10N.getStr("netmonitor.toolbar.perf"),
    onClick: triggerSummary,
  },
  span({ className: "summary-info-icon" }),
  span({ className: "summary-info-text" }, text));
}

SummaryButton.propTypes = {
  summary: PropTypes.object.isRequired,
  timingMarkers: PropTypes.object.isRequired,
};

module.exports = connect(
  (state) => ({
    summary: getDisplayedRequestsSummary(state),
    timingMarkers: {
      DOMContentLoaded:
        getDisplayedTimingMarker(state, "firstDocumentDOMContentLoadedTimestamp"),
      load: getDisplayedTimingMarker(state, "firstDocumentLoadTimestamp"),
    },
  }),
  (dispatch) => ({
    triggerSummary: () => {
      dispatch(Actions.openStatistics(true));
    },
  })
)(SummaryButton);
