set auto_path [concat [file normalize [file dirname [info script]]/../..] $auto_path]

package require xomti_memory 0.1

set m [Xomti_Memory calloc 10 8]

Xomti_Memory write_double $m 3.1
Xomti_Memory write_double [expr {$m + 8}] 3.2


puts [Xomti_Memory read_double $m]
puts [Xomti_Memory read_double [expr {$m + 8}]]

