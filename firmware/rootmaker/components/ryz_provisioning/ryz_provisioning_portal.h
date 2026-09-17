#pragma once

/* Self-contained offline portal rendered by phones and desktop browsers while
 * connected to the Ryzobee SoftAP.  It intentionally has no external assets or
 * network dependencies: captive-portal clients must be able to render it
 * before they have Internet access. */
extern const char ryz_provisioning_portal_html[];
/* Shared same-origin CSS includes vendored, OFL-licensed Latin web fonts. */
extern const char ryz_provisioning_web_css[];
extern const char ryz_diagnostics_html[];
