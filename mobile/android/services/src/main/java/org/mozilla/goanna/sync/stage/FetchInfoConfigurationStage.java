/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna.sync.stage;

import org.mozilla.goanna.sync.ExtendedJSONObject;
import org.mozilla.goanna.sync.InfoConfiguration;
import org.mozilla.goanna.sync.JSONRecordFetcher;
import org.mozilla.goanna.sync.delegates.JSONRecordFetchDelegate;
import org.mozilla.goanna.sync.net.AuthHeaderProvider;
import org.mozilla.goanna.sync.net.SyncStorageResponse;

/**
 * Fetches configuration data from info/configurations endpoint.
 */
public class FetchInfoConfigurationStage extends AbstractNonRepositorySyncStage {
    private final String configurationURL;
    private final AuthHeaderProvider authHeaderProvider;

    public FetchInfoConfigurationStage(final String configurationURL, final AuthHeaderProvider authHeaderProvider) {
        super();
        this.configurationURL = configurationURL;
        this.authHeaderProvider = authHeaderProvider;
    }

    public class StageInfoConfigurationDelegate implements JSONRecordFetchDelegate {
        @Override
        public void handleSuccess(final ExtendedJSONObject result) {
            session.config.infoConfiguration = new InfoConfiguration(result);
            session.advance();
        }

        @Override
        public void handleFailure(final SyncStorageResponse response) {
            // Handle all non-404 failures upstream.
            if (response.getStatusCode() != 404) {
                session.handleHTTPError(response, "Failure fetching info/configuration");
                return;
            }

            // End-point might not be available (404) if server is running an older version.
            // We will use default config values in this case.
            session.config.infoConfiguration = new InfoConfiguration();
            session.advance();
        }

        @Override
        public void handleError(final Exception e) {
            session.abort(e, "Failure fetching info/configuration");
        }
    }
    @Override
    public void execute() {
        final StageInfoConfigurationDelegate delegate = new StageInfoConfigurationDelegate();
        final JSONRecordFetcher fetcher = new JSONRecordFetcher(configurationURL, authHeaderProvider);
        fetcher.fetch(delegate);
    }
}
