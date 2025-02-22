# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

<!-- Sync panel -->

<preferences id="syncEnginePrefs" hidden="true" data-category="paneSync">
  <preference id="engine.addons"
              name="services.sync.engine.addons"
              type="bool"/>
  <preference id="engine.bookmarks"
              name="services.sync.engine.bookmarks"
              type="bool"/>
  <preference id="engine.history"
              name="services.sync.engine.history"
              type="bool"/>
  <preference id="engine.tabs"
              name="services.sync.engine.tabs"
              type="bool"/>
  <preference id="engine.prefs"
              name="services.sync.engine.prefs"
              type="bool"/>
  <preference id="engine.passwords"
              name="services.sync.engine.passwords"
              type="bool"/>
</preferences>

<script type="application/javascript"
        src="chrome://browser/content/preferences/sync.js"/>
<script type="application/javascript"
        src="chrome://browser/content/sync/utils.js"/>

<hbox id="header-sync"
      class="header"
      hidden="true"
      data-category="paneSync">
  <label class="header-name" flex="1">&paneSync.title;</label>
  <html:a class="help-button text-link" target="_blank" aria-label="&helpButton.label;"></html:a>
</hbox>

<deck id="weavePrefsDeck" data-category="paneSync" hidden="true">
  <!-- These panels are for the "legacy" sync provider -->
  <vbox id="noAccount" align="center">
    <spacer flex="1"/>
    <description id="syncDesc">
      &weaveDesc.label;
    </description>
    <separator/>
    <label id="noAccountSetup" class="text-link">
      &setupButton.label;
    </label>
    <vbox id="pairDevice">
      <separator/>
      <label id="noAccountPair" class="text-link">
        &pairDevice.label;
      </label>
    </vbox>
    <spacer flex="3"/>
  </vbox>

  <vbox id="hasAccount">
    <groupbox class="syncGroupBox">
      <!-- label is set to account name -->
      <caption id="accountCaption" align="center">
        <image id="accountCaptionImage"/>
        <label id="accountName"/>
      </caption>

      <hbox>
        <button type="menu"
                label="&manageAccount.label;"
                accesskey="&manageAccount.accesskey;">
          <menupopup>
            <menuitem id="syncChangePassword" label="&changePassword2.label;"/>
            <menuitem id="syncResetPassphrase" label="&myRecoveryKey.label;"/>
            <menuseparator/>
            <menuitem id="syncReset" label="&resetSync2.label;"/>
          </menupopup>
        </button>
      </hbox>

      <hbox>
        <label id="syncAddDeviceLabel"
               class="text-link">
          &pairDevice.label;
        </label>
      </hbox>

      <vbox>
        <label>&syncMy.label;</label>
        <richlistbox id="syncEnginesList"
                     orient="vertical">
          <richlistitem>
            <checkbox label="&engine.addons.label;"
                      accesskey="&engine.addons.accesskey;"
                      preference="engine.addons"/>
          </richlistitem>
          <richlistitem>
            <checkbox label="&engine.bookmarks.label;"
                      accesskey="&engine.bookmarks.accesskey;"
                      preference="engine.bookmarks"/>
          </richlistitem>
          <richlistitem>
            <checkbox label="&engine.passwords.label;"
                      accesskey="&engine.passwords.accesskey;"
                      preference="engine.passwords"/>
          </richlistitem>
          <richlistitem>
            <checkbox label="&engine.prefs.label;"
                      accesskey="&engine.prefs.accesskey;"
                      preference="engine.prefs"/>
          </richlistitem>
          <richlistitem>
            <checkbox label="&engine.history.label;"
                      accesskey="&engine.history.accesskey;"
                      preference="engine.history"/>
          </richlistitem>
          <richlistitem>
            <checkbox label="&engine.tabs.label2;"
                      accesskey="&engine.tabs.accesskey;"
                      preference="engine.tabs"/>
          </richlistitem>
        </richlistbox>
      </vbox>
    </groupbox>

    <groupbox class="syncGroupBox">
      <grid>
        <columns>
          <column/>
          <column flex="1"/>
        </columns>
        <rows>
          <row align="center">
            <label control="syncComputerName">
              &syncDeviceName.label;
            </label>
            <textbox id="syncComputerName"/>
          </row>
        </rows>
      </grid>
      <hbox>
        <label id="unlinkDevice" class="text-link">
          &unlinkDevice.label;
        </label>
      </hbox>
    </groupbox>
    <vbox id="tosPP-normal">
      <label id="tosPP-normal-ToS" class="text-link">
        &prefs.tosLink.label;
      </label>
      <label id="tosPP-normal-PP" class="text-link">
        &prefs.ppLink.label;
      </label>
    </vbox>
  </vbox>

  <vbox id="needsUpdate" align="center" pack="center">
    <hbox>
      <label id="loginError"/>
      <label id="loginErrorUpdatePass" class="text-link">
        &updatePass.label;
      </label>
      <label id="loginErrorResetPass" class="text-link">
        &resetPass.label;
      </label>
    </hbox>
    <label id="loginErrorStartOver" class="text-link">
      &unlinkDevice.label;
    </label>
  </vbox>

  <!-- These panels are for the Firefox Accounts identity provider -->
  <vbox id="noFxaAccount">
    <hbox>
      <vbox id="fxaContentWrapper">
        <groupbox id="noFxaGroup">
          <vbox>
            <label id="noFxaCaption">&signedOut.caption;</label>
            <description id="noFxaDescription" flex="1">&signedOut.description;</description>
            <hbox class="fxaAccountBox">
              <vbox>
                <image class="fxaFirefoxLogo"/>
              </vbox>
              <vbox flex="1">
                <label id="signedOutAccountBoxTitle">&signedOut.accountBox.title;</label>
                <hbox class="fxaAccountBoxButtons">
                  <button id="noFxaSignUp" label="&signedOut.accountBox.create;" accesskey="&signedOut.accountBox.create.accesskey;"></button>
                  <button id="noFxaSignIn" label="&signedOut.accountBox.signin;" accesskey="&signedOut.accountBox.signin.accesskey;"></button>
                </hbox>
              </vbox>
            </hbox>
          </vbox>
        </groupbox>
      </vbox>
      <vbox>
        <image class="fxaSyncIllustration"/>
      </vbox>
    </hbox>
  </vbox>

  <vbox id="hasFxaAccount">
    <hbox>
      <vbox id="fxaContentWrapper">
        <groupbox id="fxaGroup">
          <caption><label>&syncBrand.fxAccount.label;</label></caption>
          <deck id="fxaLoginStatus">

            <!-- logged in and verified and all is good -->
            <hbox id="fxaLoginVerified" class="fxaAccountBox">
              <vbox align="center" pack="center">
                <image id="fxaProfileImage" class="actionable"
                    role="button"
                    onclick="gSyncPane.openChangeProfileImage(event);" hidden="true"
                    onkeypress="gSyncPane.openChangeProfileImage(event);"
                    tooltiptext="&profilePicture.tooltip;"/>
              </vbox>
              <vbox flex="1" pack="center">
                <label id="fxaDisplayName" hidden="true"/>
                <label id="fxaEmailAddress1"/>
                <hbox class="fxaAccountBoxButtons">
                  <button id="fxaUnlinkButton" label="&disconnect.label;" accesskey="&disconnect.accesskey;"/>
                  <html:a id="verifiedManage" target="_blank"
                         accesskey="&verifiedManage.accesskey;"
                         onkeypress="gSyncPane.openManageFirefoxAccount(event);"><!--
                  -->&verifiedManage.label;</html:a>
                </hbox>
              </vbox>
            </hbox>

            <!-- logged in to an unverified account -->
            <hbox id="fxaLoginUnverified" class="fxaAccountBox">
              <vbox>
                <image id="fxaProfileImage"/>
              </vbox>
              <vbox flex="1">
                <hbox>
                  <vbox><image id="fxaLoginRejectedWarning"/></vbox>
                  <description flex="1">
                    &signedInUnverified.beforename.label;
                    <label id="fxaEmailAddress2"/>
                    &signedInUnverified.aftername.label;
                  </description>
                </hbox>
                <hbox class="fxaAccountBoxButtons">
                  <button id="verifyFxaAccount" label="&verify.label;" accesskey="&verify.accesskey;"></button>
                  <button id="unverifiedUnlinkFxaAccount" label="&forget.label;" accesskey="&forget.accesskey;"></button>
                </hbox>
              </vbox>
            </hbox>

            <!-- logged in locally but server rejected credentials -->
            <hbox id="fxaLoginRejected" class="fxaAccountBox">
              <vbox>
                <image id="fxaProfileImage"/>
              </vbox>
              <vbox flex="1">
                <hbox>
                  <vbox><image id="fxaLoginRejectedWarning"/></vbox>
                  <description flex="1">
                    &signedInLoginFailure.beforename.label;
                    <label id="fxaEmailAddress3"/>
                    &signedInLoginFailure.aftername.label;
                  </description>
                </hbox>
                <hbox class="fxaAccountBoxButtons">
                  <button id="rejectReSignIn" label="&signIn.label;" accesskey="&signIn.accesskey;"></button>
                  <button id="rejectUnlinkFxaAccount" label="&forget.label;" accesskey="&forget.accesskey;"></button>
                </hbox>
              </vbox>
            </hbox>
          </deck>
        </groupbox>
        <groupbox id="syncOptions">
          <caption><label>&signedIn.engines.label;</label></caption>
          <hbox id="fxaSyncEngines">
            <vbox align="start" flex="1">
              <checkbox label="&engine.tabs.label2;"
                        accesskey="&engine.tabs.accesskey;"
                        preference="engine.tabs"/>
              <checkbox label="&engine.bookmarks.label;"
                        accesskey="&engine.bookmarks.accesskey;"
                        preference="engine.bookmarks"/>
              <checkbox label="&engine.passwords.label;"
                        accesskey="&engine.passwords.accesskey;"
                        preference="engine.passwords"/>
            </vbox>
            <vbox align="start" flex="1">
              <checkbox label="&engine.history.label;"
                        accesskey="&engine.history.accesskey;"
                        preference="engine.history"/>
              <checkbox label="&engine.addons.label;"
                        accesskey="&engine.addons.accesskey;"
                        preference="engine.addons"/>
              <checkbox label="&engine.prefs.label;"
                        accesskey="&engine.prefs.accesskey;"
                        preference="engine.prefs"/>
            </vbox>
            <spacer/>
          </hbox>
        </groupbox>
      </vbox>
      <vbox>
        <image class="fxaSyncIllustration"/>
      </vbox>
    </hbox>
    <groupbox>
      <caption>
        <label control="fxaSyncComputerName">
          &fxaSyncDeviceName.label;
        </label>
      </caption>
      <hbox id="fxaDeviceName">
        <textbox id="fxaSyncComputerName" disabled="true"/>
        <hbox>
          <button id="fxaChangeDeviceName"
                  label="&changeSyncDeviceName.label;"
                  accesskey="&changeSyncDeviceName.accesskey;"/>
          <button id="fxaCancelChangeDeviceName"
                  label="&cancelChangeSyncDeviceName.label;"
                  accesskey="&cancelChangeSyncDeviceName.accesskey;"
                  hidden="true"/>
          <button id="fxaSaveChangeDeviceName"
                  label="&saveChangeSyncDeviceName.label;"
                  accesskey="&saveChangeSyncDeviceName.accesskey;"
                  hidden="true"/>
        </hbox>
      </hbox>
    </groupbox>
    <label class="fxaMobilePromo">
        &mobilePromo3.start;<!-- We put these comments to avoid inserting white spaces
        --><label class="androidLink text-link" id="fxaMobilePromo-android-hasFxaAccount"><!--
        -->&mobilePromo3.androidLink;</label><!--
        -->&mobilePromo3.iOSBefore;<!--
        --><label class="iOSLink text-link" id="fxaMobilePromo-ios-hasFxaAccount"><!--
        -->&mobilePromo3.iOSLink;</label><!--
        -->&mobilePromo3.end;
    </label>
    <vbox id="tosPP-small" align="start">
      <label id="tosPP-small-ToS" class="text-link">
        &prefs.tosLink.label;
      </label>
      <label id="tosPP-small-PP" class="text-link">
        &fxaPrivacyNotice.link.label;
      </label>
    </vbox>
  </vbox>
</deck>
