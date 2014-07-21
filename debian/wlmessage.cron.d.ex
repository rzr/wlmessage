#
# Regular cron jobs for the wlmessage package
#
0 4	* * *	root	[ -x /usr/bin/wlmessage_maintenance ] && /usr/bin/wlmessage_maintenance
