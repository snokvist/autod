package app

import "time"

// NowUTC wraps time.Now in UTC for easier testing/injection later.
func NowUTC() time.Time {
	return time.Now().UTC()
}
