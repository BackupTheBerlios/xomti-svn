set here [file dir [info script]]
puts stderr "here = $here"
pkg_mkIndex -verbose $here xomti*/tags/*/*.xotcl xomti*/trunk/*.xotcl
