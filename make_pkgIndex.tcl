set here [file dir [info script]]
puts stderr "not good" ; exit
pkg_mkIndex -verbose $here xomti*/tags/*/*.xotcl xomti*/trunk/*.xotcl antirez_tclref/tags/*/*.xotcl antirez_tclref/tags/trunk/*.xotcl

