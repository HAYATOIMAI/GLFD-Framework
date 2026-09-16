/**
 * @file  control_no_code.cpp
 * @brief T-ECS-27 control: a probe that generates no code must not read as CLEAN.
 *
 * @details
 *  **This control exists because the audit already lied to me once.** A probe
 *  whose only function took a parameter of internal-linkage type produced an
 *  object file with **zero defined function symbols** - `KickJob` was not even
 *  referenced - and the audit happily reported CLEAN. Nothing had been checked.
 *
 *  So every row now also counts defined function symbols, and a row with none
 *  measures NOCODE instead of CLEAN. This file is the tooth for that counter:
 *  it declares things and defines nothing reachable.
 *
 *  4.1 again - a green row and a working check are different claims.
 */
// EXPECT: NOCODE
// WHY: nothing here is code generated. a CLEAN verdict for this file would be a lie.

void Declared();

template <class T>
T NeverInstantiated(T value) { throw value; }

extern int g_value;
