/**
 * @file  control_clean.cpp
 * @brief T-ECS-27 control: neither check must fire on code that uses no exceptions.
 *
 * If this row is not CLEAN, the audit is reporting on something other than the
 * probe (a stale object file, a wrong include path), and every other row is suspect.
 */
// EXPECT: CLEAN
// WHY: plain arithmetic. no try, no catch, no throw.

int Add(int a, int b) { return a + b; }
