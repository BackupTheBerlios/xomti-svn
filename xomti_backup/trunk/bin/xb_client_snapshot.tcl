#!/bin/sh
# The next line is executed by /bin/sh, but not tcl \
exec tclsh8.5 "$0" ${1+"$@"}

set DF LINUX
# set DF SOLARIS

#	set GLOB(excludeRegexpDirAbsolu) [list {/export/home/Free$} {/export/home/[^/]+/C$} {/\.netscape/cache} {^/BB$} {^/local/Y}]
	set GLOB(excludeRegexpDirAbsolu) {}
set GLOB(excludeRegexpDirRelatif) [list {^tmp$} {^Poubelle$} {^Z$} {^Z_} {^TT_DB$}]
# {^\.}
# {^Y$}

package require Tcl 8.4 ;# For "wide integers", but tclsh8.4 "file ..." is too slow
package require xomti_alladin_md5 1.0
package require Db_tcl 4.3

namespace eval ::xomti::backup {}

proc ::xomti::backup::syntaxError {} {
    global argv0
    puts stderr "usage : $argv0 \[-debug\] /export/home ou autre"
    exit 1
}

proc ::xomti::backup::traiteErreur {h message} {
    puts stderr "    $message"
    puts $h $message
    flush $h
}

proc ::xomti::backup::file_lstat {f vName} {
    upvar $vName v
    return [file lstat $f v]
}

proc ::xomti::backup::file_attributes {f quoi} {
    return [file attributes $f $quoi]
}

proc ::xomti::backup::file_readlink {f} {
    return [file readlink $f]
}

proc ::xomti::backup::file_type {f} {
    return [file type $f]
}


proc ::xomti::backup::explore {dir dev &GLOB} {
    upvar ${&GLOB} GLOB

    if {0 && $GLOB(size) > 1e9} {
	$GLOB(dbLinks)       close
	$GLOB(dbDirs)        close
	$GLOB(dbFiles)       close
	$GLOB(dbDirContents) close
	$GLOB(dbInodes)      close
	close $GLOB(fileLog)
	close $GLOB(fileErrors)
	exit
    }

    puts stderr "[format %.3f [expr {$GLOB(size)/1e9}]] ::xomti::backup::explore $dir"
    set err [catch {cd $dir} message]
    if {$err} {
        ::xomti::backup::traiteErreur $GLOB(fileErrors) "cd $dir : $message"
        return
    }
    set fichiers [lsort [glob -nocomplain .* *]]
    foreach f $fichiers {
        # on saute . et ..
        if {$f == "." || $f == ".."} {
            continue
        }
        set fullname [file join $dir $f]
        if {[string index $f 0] == "~"} {
            ::xomti::backup::traiteErreur $GLOB(fileErrors) "fichier débutant par \"~\" exclu : \"$fullname\""
            continue
        }
        # lstat important pour le pas suivre les liens
        set err [catch {::xomti::backup::file_lstat $f attrib} message]
        # puts stderr $message
        # parray attrib
        if {$err} {
            traiteErreur $GLOB(fileErrors) "::xomti::backup::file_lstat \"$fullname\" : $message"
            continue
        }
        switch $attrib(type) {
            "directory" {
                $GLOB(dbDirs) put $fullname [list $attrib(mtime) $attrib(ino) [::xomti::backup::file_attributes $f -owner] [::xomti::backup::file_attributes $f -group] [::xomti::backup::file_attributes $f -permissions]]
                $GLOB(dbInodes) put $attrib(ino) [list dir $fullname]
                $GLOB(dbDirContents) put $dir [list dir $fullname]
                set exclu 0
                foreach regexp $GLOB(excludeRegexpDirAbsolu) {
                    if {[regexp $regexp $fullname]} {
                        ::xomti::backup::traiteErreur $GLOB(fileErrors) "exclu dir absolu : \"$fullname\""
                        set exclu 1
                        break
                    } else {
                        # puts stderr [list regexp $regexp $fullname -> 1]
                    }
                }
                if {$exclu} continue
                foreach regexp $GLOB(excludeRegexpDirRelatif) {
                    if {[regexp $regexp $f]} {
                        ::xomti::backup::traiteErreur $GLOB(fileErrors) "exclu dir relatif : \"$fullname\""
                        set exclu 1
                        break
                    }
                }
                if {$exclu} continue
                if {$attrib(dev) != $dev} {
                    ::xomti::backup::traiteErreur $GLOB(fileErrors) "on other device : \"$fullname\""
                    continue
                }
                ::xomti::backup::explore $fullname $dev GLOB
                $GLOB(dbLinks) sync
                $GLOB(dbDirs) sync
                $GLOB(dbFiles) sync
                $GLOB(dbDirContents) sync
                $GLOB(dbInodes) sync
                cd $dir
            }
            "file" {
                if {[regexp {[\r\n]} $f]} {
                    ::xomti::backup::traiteErreur $GLOB(fileErrors) "Fichier interdit, le nom contient un retour : \"[file join [pwd] $f]\""
                    continue
                }
                if {[string index $f 0] == "|"} {
                    ::xomti::backup::traiteErreur $GLOB(fileErrors) "Fichier interdit, le nom commence par \"|\" : \"[file join [pwd] $f]\""
                    continue
                }
                # incr ne marche pas avec un incrément "wide"
                set GLOB(size) [expr {$GLOB(size) + $attrib(size)}]
                set err [catch {::xomti::alladin_md5::file $f} md5sum]
                if {$err} {
                    ::xomti::backup::traiteErreur $GLOB(fileErrors) "xomti::alladin_md5 $f : $md5sum"
                    continue
                }
                set instant [clock seconds]
                set key "$md5sum $instant"

                if {[$GLOB(dbFiles) get $key] != {}} {
                    set iv 1
                    set key "$md5sum $instant #$iv"
                    while {[$GLOB(dbFiles) get $key] != {}} {
                        incr iv
                        set key "$md5sum $instant #$iv"
                    }
                }
                $GLOB(dbFiles) put $key [list \
					     $attrib(size) $attrib(mtime) $attrib(ino) $dir $f \
					     [::xomti::backup::file_attributes $f -owner] [::xomti::backup::file_attributes $f -group] [::xomti::backup::file_attributes $f -permissions]]
                $GLOB(dbInodes) put $attrib(ino) [list file $key]
                $GLOB(dbDirContents) put $dir [list file $key]
            }
            "link" {
                set err [catch {::xomti::backup::file_readlink $f} lili]
                if {$err} {
                    ::xomti::backup::traiteErreur $GLOB(fileErrors) "$fullname : $lili"
                } else {
                    catch {::xomti::backup::file_type $lili} lilitype
                }
                $GLOB(dbLinks) put $fullname [list $attrib(mtime) $attrib(ino) $lilitype $lili]
                $GLOB(dbInodes) put $attrib(ino) [list link $fullname]
                $GLOB(dbDirContents) put $dir [list link $fullname]
            }
            default {
                ::xomti::backup::traiteErreur $GLOB(fileErrors) "unknown type : $attrib(type) for \"$fullname\""
            }
        } 
    }
}

set GLOB(size) [expr {wide(0)}]
set GLOB(links) [list]

if {[lindex $argv 0] == "-debug"} {
    set DEBUG 1
    set argv [lrange $argv 1 end]
} else {
    set DEBUG 0
}

if {[llength $argv] != 1} {
    syntaxError
}

set fs $argv

set err [catch [list exec df -k $fs] message]
if {$err} {
    puts stderr "df -k $fs -> $message"
    exit 1
}

set lignes [split $message \n]
if {[llength $lignes] != 2} {
    puts stderr "attendu deux lignes à \"df -k $fs\" -> $message"
    exit 1
}

set ligne [lindex $lignes 1]

switch $DF {
    LINUX {set colfs 5}
    SOLARIS {set colfs 5}
    default {return -code error "DF \"$DF\" inconnu"}
}

set l [lindex $ligne $colfs]
if {$l != $fs} {
    puts stderr "attendu \"$fs\" colonne [expr {$colfs+1}] de \"$ligne\""
    # exit 1
}

set GLOB(raw) [lindex [split [lindex $ligne 0] /] end]

set GLOB(hostname) [info hostname]
set GLOB(date) [clock format [clock seconds] -format %Y-%m-%d]

set GLOB(dirdest) [file join /local/home/bb $GLOB(date).$GLOB(raw)]
if {[file exists $GLOB(dirdest)]} {
    puts stderr "dirname $GLOB(dirdest) alread exists, BYE"
    exit 1
}

parray GLOB
file mkdir $GLOB(dirdest)
file attributes $GLOB(dirdest) -group p10admin -permissions 040770

set prefix $GLOB(date).$GLOB(hostname).$GLOB(raw)
set GLOB(dbLinks)       [berkdb open -create -btree -- [file join $GLOB(dirdest) $prefix.links.db]]
set GLOB(dbDirs)        [berkdb open -create -btree -- [file join $GLOB(dirdest) $prefix.dirs.db]]
set GLOB(dbFiles)       [berkdb open -create -btree -- [file join $GLOB(dirdest) $prefix.files.db]]
set GLOB(dbDirContents) [berkdb open -create -btree -dup -- [file join $GLOB(dirdest) $prefix.dircontents.db]]
set GLOB(dbInodes)      [berkdb open -create -btree -dup -- [file join $GLOB(dirdest) $prefix.inodes.db]]
set GLOB(fileLog)    [open [file join $GLOB(dirdest) $prefix.log] w]
set GLOB(fileErrors) [open [file join $GLOB(dirdest) $prefix.errors] w]
close [open [file join $GLOB(dirdest) $prefix.start] w]

file stat $fs attrib
explore $fs $attrib(dev) GLOB

puts stderr "\n"
puts "taille traitée = [expr {1e-9*$GLOB(size)}]"

$GLOB(dbLinks) close
$GLOB(dbDirs) close
$GLOB(dbFiles) close
$GLOB(dbDirContents) close
$GLOB(dbInodes) close
close $GLOB(fileLog)
close $GLOB(fileErrors)
