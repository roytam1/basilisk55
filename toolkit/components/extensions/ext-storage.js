"use strict";

var {classes: Cc, interfaces: Ci, utils: Cu} = Components;

XPCOMUtils.defineLazyModuleGetter(this, "ExtensionStorage",
                                  "resource://gre/modules/ExtensionStorage.jsm");
XPCOMUtils.defineLazyModuleGetter(this, "AddonManagerPrivate",
                                  "resource://gre/modules/AddonManager.jsm");

Cu.import("resource://gre/modules/ExtensionUtils.jsm");
var {
  EventManager,
  ExtensionError,
} = ExtensionUtils;

function enforceNoTemporaryAddon(extensionId) {
  const EXCEPTION_MESSAGE =
        "The storage API will not work with a temporary addon ID. " +
        "Please add an explicit addon ID to your manifest. " +
        "For more information see https://bugzil.la/1323228.";
  if (AddonManagerPrivate.isTemporaryInstallID(extensionId)) {
    throw new ExtensionError(EXCEPTION_MESSAGE);
  }
}

function storageApiFactory(context) {
  let {extension} = context;
  return {
    storage: {
      local: {
        get: function(spec) {
          return ExtensionStorage.get(extension.id, spec);
        },
        set: function(items) {
          return ExtensionStorage.set(extension.id, items, context);
        },
        remove: function(keys) {
          return ExtensionStorage.remove(extension.id, keys);
        },
        clear: function() {
          return ExtensionStorage.clear(extension.id);
        },
      },

      onChanged: new EventManager(context, "storage.onChanged", fire => {
        let listenerLocal = changes => {
          fire(changes, "local");
        };

        ExtensionStorage.addOnChangedListener(extension.id, listenerLocal);
        return () => {
          ExtensionStorage.removeOnChangedListener(extension.id, listenerLocal);
        };
      }).api(),
    },
  };
}
extensions.registerSchemaAPI("storage", "addon_parent", storageApiFactory);
extensions.registerSchemaAPI("storage", "content_parent", storageApiFactory);
extensions.registerSchemaAPI("storage", "devtools_parent", storageApiFactory);
