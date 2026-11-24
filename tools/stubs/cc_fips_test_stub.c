/* Copyright (c) 2026 Darin. MIT License. Written by Claude (Anthropic) for the ios-sweet Galaxy A8 port.
 * Stand-in for /usr/libexec/cc_fips_test on translated (a8xlate) roots: the real one HMACs corecrypto's own
 * machine code (FIPS integrity self-test), which the translator rewrites, so it always fails and launchd
 * panics on the failed "fips" boot task. The crypto itself is unchanged in behaviour. */
int main(void) { return 0; }
