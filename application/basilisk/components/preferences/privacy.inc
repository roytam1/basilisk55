# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

<!-- Privacy panel -->

<script type="application/javascript"
        src="chrome://browser/content/preferences/privacy.js"/>

<preferences id="privacyPreferences" hidden="true" data-category="panePrivacy">

  <!-- Global Privacy Control -->
  <preference id="privacy.GPCheader.enabled"
              name="privacy.GPCheader.enabled"
              type="bool"/>

  <!-- XXX button prefs -->
  <preference id="pref.privacy.disable_button.cookie_exceptions"
              name="pref.privacy.disable_button.cookie_exceptions"
              type="bool"/>
  <preference id="pref.privacy.disable_button.view_cookies"
              name="pref.privacy.disable_button.view_cookies"
              type="bool"/>
  <preference id="pref.privacy.disable_button.change_blocklist"
              name="pref.privacy.disable_button.change_blocklist"
              type="bool"/>
  <preference id="pref.privacy.disable_button.tracking_protection_exceptions"
              name="pref.privacy.disable_button.tracking_protection_exceptions"
              type="bool"/>

  <!-- Location Bar -->
  <preference id="browser.urlbar.autocomplete.enabled"
              name="browser.urlbar.autocomplete.enabled"
              type="bool"/>
  <preference id="browser.urlbar.suggest.bookmark"
              name="browser.urlbar.suggest.bookmark"
              type="bool"/>
  <preference id="browser.urlbar.suggest.history"
              name="browser.urlbar.suggest.history"
              type="bool"/>
  <preference id="browser.urlbar.suggest.openpage"
              name="browser.urlbar.suggest.openpage"
              type="bool"/>

  <!-- History -->
  <preference id="places.history.enabled"
              name="places.history.enabled"
              type="bool"/>
  <preference id="browser.formfill.enable"
              name="browser.formfill.enable"
              type="bool"/>
  <preference id="privacy.history.custom"
              name="privacy.history.custom"
              type="bool"/>
  <!-- Cookies -->
  <preference id="network.cookie.cookieBehavior"
              name="network.cookie.cookieBehavior"
              type="int"/>
  <preference id="network.cookie.lifetimePolicy"
              name="network.cookie.lifetimePolicy"
              type="int"/>
  <preference id="network.cookie.blockFutureCookies"
              name="network.cookie.blockFutureCookies"
              type="bool"/>
  <!-- Clear Private Data -->
  <preference id="privacy.sanitize.sanitizeOnShutdown"
              name="privacy.sanitize.sanitizeOnShutdown"
              type="bool"/>
  <preference id="privacy.sanitize.timeSpan"
              name="privacy.sanitize.timeSpan"
              type="int"/>
  <!-- Private Browsing -->
  <preference id="browser.privatebrowsing.autostart"
              name="browser.privatebrowsing.autostart"
              type="bool"/>
</preferences>

<hbox id="header-privacy"
      class="header"
      hidden="true"
      data-category="panePrivacy">
  <label class="header-name" flex="1">&panePrivacy.title;</label>
  <html:a class="help-button" target="_blank" aria-label="&helpButton.label;"></html:a>
</hbox>

<!-- Global Privacy Control -->
<groupbox id="dataPrivacyGroup" data-category="panePrivacy" hidden="true">
  <vbox id="privacygpcbox">
    <caption><label>&dataPrivacy.label;</label></caption>
    <hbox align="center">
      <checkbox id="GPC"
                preference="privacy.GPCheader.enabled"
                accesskey="&GPC.accesskey;"
                label="&GPCLabel.label;" />
      <label class="text-link"
             value="&GPCLearnMore.label;"
             href="https://www.palemoon.org/support/global-privacy-control"/>
    </hbox>
  </vbox>
</groupbox>

<!-- History -->
<groupbox id="historyGroup" data-category="panePrivacy" hidden="true">
  <caption><label>&history.label;</label></caption>
  <hbox align="center">
    <label id="historyModeLabel"
           control="historyMode"
           accesskey="&historyHeader.pre.accesskey;">&historyHeader.pre.label;
    </label>
    <menulist id="historyMode">
      <menupopup>
        <menuitem label="&historyHeader.remember.label;" value="remember"/>
        <menuitem label="&historyHeader.dontremember.label;" value="dontremember"/>
        <menuitem label="&historyHeader.custom.label;" value="custom"/>
      </menupopup>
    </menulist>
    <label>&historyHeader.post.label;</label>
  </hbox>
  <deck id="historyPane">
    <vbox id="historyRememberPane">
      <hbox align="center" flex="1">
        <vbox flex="1">
          <description>&rememberDescription.label;</description>
          <separator class="thin"/>
          <description>&rememberActions.pre.label;<label
          class="text-link" id="historyRememberClear"
          >&rememberActions.clearHistory.label;</label>&rememberActions.middle.label;<label
          class="text-link" id="historyRememberCookies"
          >&rememberActions.removeCookies.label;</label>&rememberActions.post.label;</description>
        </vbox>
      </hbox>
    </vbox>
    <vbox id="historyDontRememberPane">
      <hbox align="center" flex="1">
        <vbox flex="1">
          <description>&dontrememberDescription.label;</description>
          <separator class="thin"/>
          <description>&dontrememberActions.pre.label;<label
          class="text-link" id="historyDontRememberClear"
          >&dontrememberActions.clearHistory.label;</label>&dontrememberActions.post.label;</description>
        </vbox>
      </hbox>
    </vbox>
    <vbox id="historyCustomPane">
      <separator class="thin"/>
      <vbox>
        <vbox align="start">
          <checkbox id="privateBrowsingAutoStart"
                    label="&privateBrowsingPermanent2.label;"
                    accesskey="&privateBrowsingPermanent2.accesskey;"
                    preference="browser.privatebrowsing.autostart"/>
        </vbox>
        <vbox class="indent">
          <vbox align="start">
            <checkbox id="rememberHistory"
                      label="&rememberHistory2.label;"
                      accesskey="&rememberHistory2.accesskey;"
                      preference="places.history.enabled"/>
            <checkbox id="rememberForms"
                      label="&rememberSearchForm.label;"
                      accesskey="&rememberSearchForm.accesskey;"
                      preference="browser.formfill.enable"/>
          </vbox>
          <hbox id="cookiesBox">
            <checkbox id="acceptCookies" label="&acceptCookies.label;"
                      preference="network.cookie.cookieBehavior"
                      accesskey="&acceptCookies.accesskey;"
                      onsyncfrompreference="return gPrivacyPane.readAcceptCookies();"
                      onsynctopreference="return gPrivacyPane.writeAcceptCookies();"/>
            <spacer flex="1" />
            <button id="cookieExceptions"
                    label="&cookieExceptions.label;" accesskey="&cookieExceptions.accesskey;"
                    preference="pref.privacy.disable_button.cookie_exceptions"/>
          </hbox>
          <hbox id="acceptThirdPartyRow"
                class="indent"
                align="center">
            <label id="acceptThirdPartyLabel" control="acceptThirdPartyMenu"
                   accesskey="&acceptThirdParty.pre.accesskey;">&acceptThirdParty.pre.label;</label>
            <menulist id="acceptThirdPartyMenu" preference="network.cookie.cookieBehavior"
            onsyncfrompreference="return gPrivacyPane.readAcceptThirdPartyCookies();"
            onsynctopreference="return gPrivacyPane.writeAcceptThirdPartyCookies();">
              <menupopup>
                <menuitem label="&acceptThirdParty.always.label;" value="always"/>
                <menuitem label="&acceptThirdParty.visited.label;" value="visited"/>
                <menuitem label="&acceptThirdParty.never.label;" value="never"/>
              </menupopup>
            </menulist>
          </hbox>
          <hbox id="keepRow"
                class="indent"
                align="center">
            <label id="keepUntil"
                   control="keepCookiesUntil"
                   accesskey="&keepUntil.accesskey;">&keepUntil.label;</label>
            <menulist id="keepCookiesUntil"
                      preference="network.cookie.lifetimePolicy">
              <menupopup>
                <menuitem label="&expire.label;" value="0"/>
                <menuitem label="&close.label;" value="2"/>
              </menupopup>
            </menulist>
            <spacer flex="1"/>
            <button id="showCookiesButton"
                    label="&showCookies.label;" accesskey="&showCookies.accesskey;"
                    preference="pref.privacy.disable_button.view_cookies"/>
          </hbox>
          <hbox id="clearDataBox"
                align="center">
            <checkbox id="alwaysClear"
                      preference="privacy.sanitize.sanitizeOnShutdown"
                      label="&clearOnClose.label;"
                      accesskey="&clearOnClose.accesskey;"/>
            <spacer flex="1"/>
            <button id="clearDataSettings" label="&clearOnCloseSettings.label;"
                    accesskey="&clearOnCloseSettings.accesskey;"/>
          </hbox>
        </vbox>
      </vbox>
    </vbox>
  </deck>
</groupbox>

<!-- Location Bar -->
<groupbox id="locationBarGroup"
          data-category="panePrivacy"
          hidden="true">
  <caption><label>&locationBar.label;</label></caption>
  <label id="locationBarSuggestionLabel">&locbar.suggest.label;</label>
  <checkbox id="historySuggestion" label="&locbar.history.label;"
            accesskey="&locbar.history.accesskey;"
            preference="browser.urlbar.suggest.history"/>
  <checkbox id="bookmarkSuggestion" label="&locbar.bookmarks.label;"
            accesskey="&locbar.bookmarks.accesskey;"
            preference="browser.urlbar.suggest.bookmark"/>
  <checkbox id="openpageSuggestion" label="&locbar.openpage.label;"
            accesskey="&locbar.openpage.accesskey;"
            preference="browser.urlbar.suggest.openpage"/>
  <label class="text-link" onclick="gotoPref('search')">
    &suggestionSettings.label;
  </label>
</groupbox>

<!-- Containers -->
<groupbox id="browserContainersGroup" data-category="panePrivacy" hidden="true">
  <vbox id="browserContainersbox" hidden="true">
    <caption><label>&browserContainersHeader.label;
      <label id="browserContainersLearnMore" class="learnMore text-link"
             value="&browserContainersLearnMore.label;"/>
    </label></caption>
    <hbox align="start">
      <vbox>
        <checkbox id="browserContainersCheckbox"
                  label="&browserContainersEnabled.label;"
                  accesskey="&browserContainersEnabled.accesskey;"
                  preference="privacy.userContext.enabled"
                  onsyncfrompreference="return gPrivacyPane.readBrowserContainersCheckbox();"/>
      </vbox>
      <spacer flex="1"/>
      <vbox>
        <button id="browserContainersSettings"
                label="&browserContainersSettings.label;"
                accesskey="&browserContainersSettings.accesskey;"/>
      </vbox>
    </hbox>
  </vbox>
</groupbox>
