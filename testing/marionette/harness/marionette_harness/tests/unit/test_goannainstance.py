# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

from marionette_driver.goannainstance import apps, GoannaInstance

from marionette_harness import MarionetteTestCase


class TestGoannaInstance(MarionetteTestCase):

    def test_create(self):
        """Test that the correct goanna instance is determined."""
        for app in apps:
            # If app has been specified we directly return the appropriate instance class
            self.assertEqual(type(GoannaInstance.create(app=app, bin="n/a")),
                             apps[app])

        # Unknown applications and binaries should fail
        self.assertRaises(NotImplementedError, GoannaInstance.create,
                          app="n/a", bin=self.marionette.bin)
        self.assertRaises(NotImplementedError, GoannaInstance.create,
                          bin="n/a")
        self.assertRaises(NotImplementedError, GoannaInstance.create,
                          bin=None)
