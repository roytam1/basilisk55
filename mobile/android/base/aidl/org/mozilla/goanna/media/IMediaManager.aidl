/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.goanna.media;

// Non-default types used in interface.
import org.mozilla.goanna.media.ICodec;
import org.mozilla.goanna.media.IMediaDrmBridge;

interface IMediaManager {
    /** Creates a remote ICodec object. */
    ICodec createCodec();

    /** Creates a remote IMediaDrmBridge object. */
    IMediaDrmBridge createRemoteMediaDrmBridge(in String keySystem,
                                               in String stubId);
}
