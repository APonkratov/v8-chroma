#pragma once
#include "protocol.h"

// The theme writer installs hooks only for the supported platform profile.
void initializeEditorSyntax(const wchar_t* platformDirectory);
void configureEditorSyntax(const ProbeConfig* config);
