set ici [file dirname [info script]]
set auto_path [concat [file normalize [pwd]/../..] $auto_path]
source $ici/fidev_critcl.xotcl
package require fidev_tcllib_md5 20050124
