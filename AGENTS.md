# Repository workflow

For each user-requested modification, verify the changes, commit them, and push to the branch's configured upstream. Follow any different instructions the user gives for a specific task.

Prefix project-owned C++ class, struct, and scoped enum names with `Sm`. Preserve exported C plugin names.

Build each plugin as an independent module. Plugin targets may include the public SDK and their own backend dependencies. Keep backend code out of the host and other plugins.
