#!/usr/local/bin/xowish
# start File for use XotclIDE as stand alone application
# please adapt the xotcl library path for your machine
# you can use tk or tix for XotclIDE
# (you can not use TixInspector without tix)

set sname [info script]
if {$sname==""} {
    # Run interactive for develop purposes
    set progdir /home/artur/xotclIDE
} else {
    file lstat $sname stats
    # follow sym links
    if {$stats(type)=="link"} {
	set sname [file readlink $sname]
	if {[file pathtype $sname]=="relative"} {
	    set sname [file join [file dirname [info script]] $sname]
	}
    }
    set xotclidedir [file dirname $sname]
}

source [file join $xotclidedir ideBase.tcl]
package require IDEVCInstaller
package require IDEStart
package require xdobry::sql

IDE::InstallerGUI installer .

