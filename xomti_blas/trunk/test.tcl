set auto_path [concat [file normalize [file dirname [info script]]/../..] $auto_path]

package require xomti_blas 0.1

Xomti_Blas_vector_double create v1 {10 20 30 40}
puts [v1 as_list]
Xomti_Blas_vector_double create v2 -length 10
