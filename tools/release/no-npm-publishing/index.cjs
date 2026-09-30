// semantic-release installs its default npm plugin even when our release config excludes it.
throw new Error('Runmark does not publish npm packages; use release.config.cjs');
