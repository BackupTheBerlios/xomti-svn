set auto_path [concat [file normalize [file dirname [info script]]/../..] $auto_path]

package require xomti_gsl 0.1
package require xomti_gsl_math 0.1

set x 5.0
set tcl_precision 17
puts "J0($x) = [Xomti_Gsl::_sf_bessel_J0_ $x]"

set x 0.01
puts "log1p($x) = [Xomti_Gsl_Math::log1p $x]"

puts [Xomti_Gsl_Math::fcmp 10.0 10.000000000001 1e-6]
puts [Xomti_Gsl_Math::fcmp 10.0 10.01 1e-6]
puts $Xomti_Gsl_Math::M_E
puts $Xomti_Gsl_Math::M_LOG2E
