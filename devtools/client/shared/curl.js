/* -*- indent-tabs-mode: nil; js-indent-level: 2 -*- */
/* vim: set ft=javascript ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/*
 * Copyright (C) 2007, 2008 Apple Inc.  All rights reserved.
 * Copyright (C) 2008, 2009 Anthony Ricaud <rik@webkit.org>
 * Copyright (C) 2011 Google Inc. All rights reserved.
 * Copyright (C) 2009 Mozilla Foundation. All rights reserved.
 * Copyright (C) 2022, 2023, 2025 Moonchild Productions. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

"use strict";

const Services = require("Services");

const DEFAULT_HTTP_VERSION = "HTTP/1.1";

const Curl = {
  /**
   * Generates a cURL command string which can be used from the command line etc.
   *
   * @param object data
   *        Datasource to create the command from.
   *        The object must contain the following properties:
   *        - url:string, the URL of the request.
   *        - method:string, the request method upper cased. HEAD / GET / POST etc.
   *        - headers:array, an array of request headers {name:x, value:x} tuples.
   *        - httpVersion:string, http protocol version rfc2616 formatted. Eg. "HTTP/1.1"
   *        - postDataText:string, optional - the request payload.
   *
   * @return string
   *         A cURL command.
   */
  generateCommand: function (data) {
    const utils = CurlUtils;

    let commandParts = [];

    // Make sure to use the following helpers to sanitize arguments before execution.
    const escapeStringifNeeded = value => {
      return /^[a-zA-Z-]+$/.test(value) ? value : escapeString(value);
    };

    let ignoredHeaders = new Set();

    // The cURL command is expected to run on the same platform that Firefox runs
    // (it may be different from the inspected page platform).
    let escapeString = Services.appinfo.OS == "WINNT" ?
                       utils.escapeStringWin : utils.escapeStringPosix;

    // Add URL.
    commandParts.push(data.url);

    // Disable globbing if the URL contains brackets.
    // cURL also globs braces but they are already percent-encoded.
    if (data.url.includes("[") || data.url.includes("]")) {
      commandParts.push("--globoff");
    }

    let postDataText = null;
    let multipartRequest = utils.isMultipartRequest(data);

    // Create post data.
    let postData = [];
    if (utils.isUrlEncodedRequest(data) ||
          ["PUT", "POST", "PATCH"].includes(data.method)) {
      postDataText = data.postDataText;
      // Irony: curl that comes with Tiger doesn't support this option.
      // But if you are l33t enough to use this, you should know that.
      // -- Cameron
      postData.push("--data-raw");
      let text =utils.writePostDataTextParams(postDataText);
      postData.push(escapeStringifNeeded(text));
      ignoredHeaders.add("content-length");
    } else if (multipartRequest) {
      postDataText = data.postDataText;
      postData.push("--data-binary");
      let boundary = utils.getMultipartBoundary(data);
      let text = utils.removeBinaryDataFromMultipartText(postDataText, boundary);
      postData.push(escapeStringifNeeded(text));
      ignoredHeaders.add("content-length");
    }

    // Add method.
    // For GET and POST requests this is not necessary as GET is the
    // default. If --data or --binary is added POST is the default.
    if (!(data.method == "GET" || data.method == "POST")) {
      commandParts.push("-X");
      commandParts.push(data.method);
    }

    // Add -I (HEAD)
    // For servers that supports HEAD.
    // This will fetch the header of a document only.
    if (data.method == "HEAD") {
      commandParts.push("-I");
    }

    // Add http version.
    if (data.httpVersion && data.httpVersion != DEFAULT_HTTP_VERSION) {
      let version = data.httpVersion.split("/")[1];
      // curl accepts --http1.0, --http1.1 and --http2 for HTTP/1.0, HTTP/1.1
      // and HTTP/2 protocols respectively. But the corresponding values in 
      // data.httpVersion are HTTP/1.0, HTTP/1.1 and HTTP/2.0
      // So in case of HTTP/2.0 (which should ideally be HTTP/2) we are using
      // only major version, and full version in other cases
      commandParts.push("--http" + (version == "2.0" ? version.split(".")[0] : version));
    }

    // Add request headers.
    let headers = data.headers;
    if (multipartRequest) {
      let multipartHeaders = utils.getHeadersFromMultipartText(postDataText);
      headers = headers.concat(multipartHeaders);
    }
    for (let i = 0; i < headers.length; i++) {
      let header = headers[i];
      if (header.name.toLowerCase() === "accept-encoding") {
        // Ignore transfer encoding (compression) as not all commonly installed
        // versions of curl support this.
        continue;
      }
      if (ignoredHeaders.has(header.name.toLowerCase())) {
        continue;
      }
      commandParts.push("-H");
      let text = header.name + ": " + header.value;
      commandParts.push(escapeStringifNeeded(text));
    }

    // Add post data.
    commandParts = commandParts.concat(postData);

    // Format with line breaks if the command has more than 2 parts
    // e.g
    // Command with 2 parts  - curl https://foo.com
    // Commands with more than 2 parts -
    // curl https://foo.com
    // -X POST
    // -H "Accept : */*"
    // -H "accept-language: en-US"
    const joinStr = Services.appinfo.OS == "WINNT" ? " ^\n  " : " \\\n  ";
    return (
      "curl " + commandParts.join(commandParts.length >= 3 ? joinStr : " ")
    );
  }
};

exports.Curl = Curl;

/**
 * Utility functions for the Curl command generator.
 */
const CurlUtils = {
  /**
   * Check if the request is an URL encoded request.
   *
   * @param object data
   *        The data source. See the description in the Curl object.
   * @return boolean
   *         True if the request is URL encoded, false otherwise.
   */
  isUrlEncodedRequest: function (data) {
    let postDataText = data.postDataText;
    if (!postDataText) {
      return false;
    }

    postDataText = postDataText.toLowerCase();
    if (postDataText.includes("content-type: application/x-www-form-urlencoded")) {
      return true;
    }

    let contentType = this.findHeader(data.headers, "content-type");

    return (contentType &&
      contentType.toLowerCase().includes("application/x-www-form-urlencoded"));
  },

  /**
   * Check if the request is a multipart request.
   *
   * @param object data
   *        The data source.
   * @return boolean
   *         True if the request is multipart reqeust, false otherwise.
   */
  isMultipartRequest: function (data) {
    let postDataText = data.postDataText;
    if (!postDataText) {
      return false;
    }

    postDataText = postDataText.toLowerCase();
    if (postDataText.includes("content-type: multipart/form-data")) {
      return true;
    }

    let contentType = this.findHeader(data.headers, "content-type");

    return (contentType &&
      contentType.toLowerCase().includes("multipart/form-data;"));
  },

  /**
   * Write out paramters from post data text.
   *
   * @param object postDataText
   *        Post data text.
   * @return string
   *         Post data parameters.
   */
  writePostDataTextParams: function (postDataText) {
    if (!postDataText) {
      return "";
    }
    let lines = postDataText.split("\r\n");
    return lines[lines.length - 1];
  },

  /**
   * Finds the header with the given name in the headers array.
   *
   * @param array headers
   *        Array of headers info {name:x, value:x}.
   * @param string name
   *        The header name to find.
   * @return string
   *         The found header value or null if not found.
   */
  findHeader: function (headers, name) {
    if (!headers) {
      return null;
    }

    name = name.toLowerCase();
    for (let header of headers) {
      if (name == header.name.toLowerCase()) {
        return header.value;
      }
    }

    return null;
  },

  /**
   * Returns the boundary string for a multipart request.
   *
   * @param string data
   *        The data source. See the description in the Curl object.
   * @return string
   *         The boundary string for the request.
   */
  getMultipartBoundary: function (data) {
    let boundaryRe = /\bboundary=(-{3,}\w+)/i;

    // Get the boundary string from the Content-Type request header.
    let contentType = this.findHeader(data.headers, "Content-Type");
    if (boundaryRe.test(contentType)) {
      return contentType.match(boundaryRe)[1];
    }
    // Temporary workaround. As of 2014-03-11 the requestHeaders array does not
    // always contain the Content-Type header for mulitpart requests. See bug 978144.
    // Find the header from the request payload.
    let boundaryString = data.postDataText.match(boundaryRe)[1];
    if (boundaryString) {
      return boundaryString;
    }

    return null;
  },

  /**
   * Removes the binary data from multipart text.
   *
   * @param string multipartText
   *        Multipart form data text.
   * @param string boundary
   *        The boundary string.
   * @return string
   *         The multipart text without the binary data.
   */
  removeBinaryDataFromMultipartText: function (multipartText, boundary) {
    let result = "";
    boundary = "--" + boundary;
    let parts = multipartText.split(boundary);
    for (let part of parts) {
      // Each part is expected to have a content disposition line.
      let contentDispositionLine = part.trimLeft().split("\r\n")[0];
      if (!contentDispositionLine) {
        continue;
      }
      contentDispositionLine = contentDispositionLine.toLowerCase();
      if (contentDispositionLine.includes("content-disposition: form-data")) {
        if (contentDispositionLine.includes("filename=")) {
          // The header lines and the binary blob is separated by 2 CRLF's.
          // Add only the headers to the result.
          let headers = part.split("\r\n\r\n")[0];
          result += boundary + "\r\n" + headers + "\r\n\r\n";
        } else {
          result += boundary + "\r\n" + part;
        }
      }
    }
    result += boundary + "--\r\n";

    return result;
  },

  /**
   * Get the headers from a multipart post data text.
   *
   * @param string multipartText
   *        Multipart post text.
   * @return array
   *         An array of header objects {name:x, value:x}
   */
  getHeadersFromMultipartText: function (multipartText) {
    let headers = [];
    if (!multipartText || multipartText.startsWith("---")) {
      return headers;
    }

    // Get the header section.
    let index = multipartText.indexOf("\r\n\r\n");
    if (index == -1) {
      return headers;
    }

    // Parse the header lines.
    let headersText = multipartText.substring(0, index);
    let headerLines = headersText.split("\r\n");
    let lastHeaderName = null;

    for (let line of headerLines) {
      // Create a header for each line in fields that spans across multiple lines.
      // Subsquent lines always begins with at least one space or tab character.
      // (rfc2616)
      if (lastHeaderName && /^\s+/.test(line)) {
        headers.push({ name: lastHeaderName, value: line.trim() });
        continue;
      }

      let indexOfColon = line.indexOf(":");
      if (indexOfColon == -1) {
        continue;
      }

      let header = [line.slice(0, indexOfColon), line.slice(indexOfColon + 1)];
      if (header.length != 2) {
        continue;
      }
      lastHeaderName = header[0].trim();
      headers.push({ name: lastHeaderName, value: header[1].trim() });
    }

    return headers;
  },

  /**
   * Escape util function for POSIX oriented operating systems.
   * Credit: Google DevTools
   */
  escapeStringPosix: function (str) {
    function escapeCharacter(x) {
      let code = x.charCodeAt(0);
      if (code < 256) {
        // Add leading zero when needed to not care about the next character.
        return code < 16 ? "\\x0" + code.toString(16) : "\\x" + code.toString(16);
      }
      code = code.toString(16);
      return "\\u" + ("0000" + code).substr(code.length, 4);
    }

    // Escape & and |, which are special characters on Windows.
    const winSpecialCharsRegEx = /([&\|])/g;

    if (/[^\x20-\x7E]|\'/.test(str)) {
      // Use ANSI-C quoting syntax.
      return "$\'" + str.replace(/\\/g, "\\\\")
                        .replace(/\'/g, "\\\'")
                        .replace(/\n/g, "\\n")
                        .replace(/\r/g, "\\r")
                        .replace(/!/g, "\\041")
                        .replace(winSpecialCharsRegEx, "^$1")
                        .replace(/[^\x20-\x7E]/g, escapeCharacter) + "'";
    }

    // Use single quote syntax.
    return "'" + str.replace(winSpecialCharsRegEx, "^$1") + "'";
  },

  /**
   * Escape util function for Windows systems.
   */
  escapeStringWin: function (str) {
    /*
       Because the cmd.exe parser and the MS Crt arguments parsers use some
       of the same escape characters, they can interact with each other in
       terrible ways, meaning the order of operations is critical here.

       1. Replace \ with \\ first, because it is an escape character for
       certain conditions in both parsers.

       2. Escape double quotes with double backslashes.

       3. Escape ` and $ so commands do not get executed, e.g $(calc.exe) or
       `\$(calc.exe)

       4. Escape all characters we are not sure about with ^, to ensure it
       gets to the MS Crt arguments parser safely.

       5. The % character is special because the MS Crt arguments parser will
       try and look for environment variables and fill them in, in-place. We
       cannot escape them with % and cannot escape them with ^ (because it's
       cmd.exe's escape, not the MS Crt arguments parser). So, we can get the
       cmd.exe parser to escape the character after it, if it is followed by
       a valid starting character of an environment variable.
       This ensures we do not try and double-escape another ^ if it was placed
       by the previous replace.

       6. Lastly, we replace new lines with ^ and TWO new lines, because the
       first new line is there to enact the escape command, and the second is
       the character to escape (in this case new line).
    */
    const encapsChars = '^"';
    return (
      encapsChars +
      str
        .replace(/\\/g, "\\\\")
        .replace(/"/g, '\\"')
        .replace(/[`$]/g, "\\$&")
        .replace(/[^a-zA-Z0-9\s_\-:=+~\/.',?;()*\$&\\{}\"`]/g, "^$&")
        .replace(/%(?=[a-zA-Z0-9_])/g, "%^")
        .replace(/\r?\n/g, "^\n\n")
      + encapsChars);
  }
};

exports.CurlUtils = CurlUtils;
