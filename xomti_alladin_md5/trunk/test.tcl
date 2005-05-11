set auto_path [concat [file normalize [file dirname [info script]]/../..] $auto_path]

package require xomti_alladin_md5 1.0

puts [::xomti::alladin_md5::file ~/.bashrc]