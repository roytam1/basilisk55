/* -*- Mode: IDL; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/.
 */

[OverrideBuiltins]
interface HTMLDocument : Document {
           [SetterThrows]
           attribute DOMString domain;
           [Throws]
           attribute DOMString cookie;
  // DOM tree accessors
  [Throws]
  getter object (DOMString name);
  [CEReactions, Pure, SetterThrows]
           attribute HTMLElement? body;
  [Pure]
  readonly attribute HTMLHeadElement? head;
  [Pure]
  readonly attribute HTMLCollection images;
  [Pure]
  readonly attribute HTMLCollection embeds;
  [Pure]
  readonly attribute HTMLCollection plugins;
  [Pure]
  readonly attribute HTMLCollection links;
  [Pure]
  readonly attribute HTMLCollection forms;
  [Pure]
  readonly attribute HTMLCollection scripts;

  // dynamic markup insertion
  [CEReactions, Throws]
  Document open(optional DOMString type = "text/html", optional DOMString replace = "");
  [CEReactions, Throws]
  WindowProxy? open(DOMString url, DOMString name, DOMString features, optional boolean replace = false);
  [CEReactions, Throws]
  void close();
  [CEReactions, Throws]
  void write(DOMString... text);
  [CEReactions, Throws]
  void writeln(DOMString... text);

  [CEReactions, SetterThrows, NeedsSubjectPrincipal]
           attribute DOMString designMode;
  [CEReactions, Throws, NeedsCallerType]
  boolean execCommand(DOMString commandId, optional boolean showUI = false,
                      optional DOMString value = "");
  [Throws, NeedsCallerType]
  boolean queryCommandEnabled(DOMString commandId);
  [Throws]
  boolean queryCommandIndeterm(DOMString commandId);
  [Throws]
  boolean queryCommandState(DOMString commandId);
  [NeedsCallerType]
  boolean queryCommandSupported(DOMString commandId);
  [Throws]
  DOMString queryCommandValue(DOMString commandId);

  [CEReactions, TreatNullAs=EmptyString] attribute DOMString fgColor;
  [CEReactions, TreatNullAs=EmptyString] attribute DOMString linkColor;
  [CEReactions, TreatNullAs=EmptyString] attribute DOMString vlinkColor;
  [CEReactions, TreatNullAs=EmptyString] attribute DOMString alinkColor;
  [CEReactions, TreatNullAs=EmptyString] attribute DOMString bgColor;

  [Pure]
  readonly attribute HTMLCollection anchors;
  [Pure]
  readonly attribute HTMLCollection applets;

  void clear();

  readonly attribute HTMLAllCollection all;

  // @deprecated These are old Netscape 4 methods. Do not use,
  //             the implementation is no-op.
  // XXXbz do we actually need these anymore?
  void                      captureEvents();
  void                      releaseEvents();
};

partial interface HTMLDocument {
  /*
   * Number of nodes that have been blocked by
   * the Safebrowsing API to prevent tracking.
   */
  [ChromeOnly, Pure]
  readonly attribute long blockedTrackingNodeCount;

  /*
   * List of nodes that have been blocked by
   * the Safebrowsing API to prevent tracking.
   */
  [ChromeOnly, Pure]
  readonly attribute NodeList blockedTrackingNodes;
};
