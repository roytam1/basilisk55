/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


[Exposed=Worker]
interface WorkerNavigator {
};

WorkerNavigator implements NavigatorID;
WorkerNavigator implements NavigatorLanguage;
WorkerNavigator implements NavigatorOnLine;
WorkerNavigator implements NavigatorConcurrentHardware;
WorkerNavigator implements NavigatorStorage;
WorkerNavigator implements NavigatorGlobalPrivacyControl;

// http://wicg.github.io/netinfo/#extensions-to-the-navigator-interface
[Exposed=(Worker)]
partial interface WorkerNavigator {
    [Func="mozilla::dom::network::Connection::IsEnabled", Throws]
    readonly attribute NetworkInformation connection;
};
