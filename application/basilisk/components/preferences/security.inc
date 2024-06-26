# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

<!-- Security panel -->

<script type="application/javascript"
        src="chrome://browser/content/preferences/security.js"/>

<preferences id="securityPreferences" hidden="true" data-category="paneSecurity">
  <!-- XXX buttons -->
  <preference id="pref.privacy.disable_button.view_passwords"
              name="pref.privacy.disable_button.view_passwords"
              type="bool"/>
  <preference id="pref.privacy.disable_button.view_passwords_exceptions"
              name="pref.privacy.disable_button.view_passwords_exceptions"
              type="bool"/>

  <!-- Add-ons -->
  <preference id="xpinstall.whitelist.required"
              name="xpinstall.whitelist.required"
              type="bool"/>

  <!-- Passwords -->
  <preference id="signon.rememberSignons" name="signon.rememberSignons" type="bool"/>
  <preference id="signon.autofillForms"   name="signon.autofillForms"   type="bool"/>

</preferences>

<hbox id="header-security"
      class="header"
      hidden="true"
      data-category="paneSecurity">
  <label class="header-name" flex="1">&paneSecurity.title;</label>
  <html:a class="help-button" target="_blank" aria-label="&helpButton.label;"></html:a>
</hbox>

<!-- addons, forgery (phishing) UI -->
<groupbox id="addonsPhishingGroup" data-category="paneSecurity" hidden="true">
  <caption><label>&general.label;</label></caption>

  <hbox id="addonInstallBox">
    <checkbox id="warnAddonInstall"
              label="&warnOnAddonInstall.label;"
              accesskey="&warnOnAddonInstall.accesskey;"
              preference="xpinstall.whitelist.required"
              onsyncfrompreference="return gSecurityPane.readWarnAddonInstall();"/>
    <spacer flex="1"/>
    <button id="addonExceptions"
            label="&addonExceptions.label;"
            accesskey="&addonExceptions.accesskey;"/>
  </hbox>

</groupbox>

<!-- Passwords -->
<groupbox id="passwordsGroup" orient="vertical" data-category="paneSecurity" hidden="true">
  <caption><label>&logins.label;</label></caption>

  <hbox id="savePasswordsBox">
    <checkbox id="savePasswords"
              label="&rememberLogins.label;" accesskey="&rememberLogins.accesskey;"
              preference="signon.rememberSignons"
              onsyncfrompreference="return gSecurityPane.readSavePasswords();"/>
    <spacer flex="1"/>
    <button id="passwordExceptions"
            label="&passwordExceptions.label;"
            accesskey="&passwordExceptions.accesskey;"
            preference="pref.privacy.disable_button.view_passwords_exceptions"/>
  </hbox>
  <checkbox id="autofillPasswords" flex="1"
         label="&autofillPasswords.label;" accesskey="&autofillPasswords.accesskey;"
         preference="signon.autofillForms"/>
  <grid id="passwordGrid">
    <columns>
      <column flex="1"/>
      <column/>
    </columns>
    <rows id="passwordRows">
      <row id="masterPasswordRow">
        <hbox id="masterPasswordBox">
          <checkbox id="useMasterPassword"
                    label="&useMasterPassword.label;"
                    accesskey="&useMasterPassword.accesskey;"/>
          <spacer flex="1"/>
        </hbox>
        <button id="changeMasterPassword"
                label="&changeMasterPassword.label;"
                accesskey="&changeMasterPassword.accesskey;"/>
      </row>
      <row id="showPasswordRow">
        <hbox id="showPasswordsBox"/>
        <button id="showPasswords"
                label="&savedLogins.label;" accesskey="&savedLogins.accesskey;"
                preference="pref.privacy.disable_button.view_passwords"/>
      </row>
    </rows>
  </grid>
</groupbox>
