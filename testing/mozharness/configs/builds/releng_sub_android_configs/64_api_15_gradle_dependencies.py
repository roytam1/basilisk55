config = {
    'base_name': 'Android armv7 API 15+ Gradle dependencies %(branch)s',
    'stage_platform': 'android-api-15-gradle-dependencies',
    'build_type': 'api-15-opt',
    'src_mozconfig': 'mobile/android/config/mozconfigs/android-api-15-gradle-dependencies/nightly',
    'tooltool_manifest_src': 'mobile/android/config/tooltool-manifests/android-gradle-dependencies/releng.manifest',
    'multi_locale_config_platform': 'android',
    'postflight_build_mach_commands': [
        ['gradle',
         'assembleAutomationRelease',
         'assembleAutomationDebug',
         'assembleAutomationDebugAndroidTest',
         'checkstyle',
         'findbugsAutomationDebug',
         # Does not include Goanna binaries -- see mobile/android/gradle/with_goanna_binaries.gradle.
         'goannaview:assembleWithoutGoannaBinaries',
         # So that we pick up the test dependencies for the builders.
         'goannaview_example:assembleWithoutGoannaBinaries',
         'goannaview_example:assembleWithoutGoannaBinariesAndroidTest',
        ],
    ],
    'artifact_flag_build_variant_in_try': None, # There's no artifact equivalent.
}
