################################################################################
#
# This are examples of simple data structures like
# linked lists and binary search trees build with tclref.
#
# Note that programming is exactly the same as with languages
# that support references in a native way.

load references.so
namespace import ::ref::* ::struct::*

################################################################################
# Binary Search Trees

struct tnode parent left right key
struct tree root

proc bst-insert {tree z} {
    set y {}
    set x [tree.root $tree]
    while {[isref $x]} {
	set y $x
	if {[tnode.key $z] < [tnode.key $x]} {
	    set x [tnode.left $x]
	} else {
	    set x [tnode.right $x]
	}
    }
    set-tnode.parent $z $y
    if {![isref $y]} {
	set-tree.root $tree $z
    } else {
	if {[tnode.key $z] < [tnode.key $y]} {
	    set-tnode.left $y $z
	} else {
	    set-tnode.right $y $z
	}
    }
}

proc bts-inorder-walk {tree} {
    bts-inorder-walk-tnode [tree.root $tree]
    puts {}
}

proc bts-inorder-walk-tnode {root} {
    if {[isref $root]} {
	bts-inorder-walk-tnode [tnode.left $root]
	puts -nonewline "[tnode.key $root] "
	bts-inorder-walk-tnode [tnode.right $root]
    }
}

# Example usage. Add 100 random numbers inside the search tree
set tree [make-tree]
for {set i 0} {$i < 100} {incr i} {
    lappend list [expr {int(rand()*100)}]
}
foreach t $list {
    set newnode [set-tnode.key [make-tnode] $t]
    bst-insert $tree $newnode
}

# Now print all the keys in-order
puts -nonewline "Tree inorder-walk output: "
bts-inorder-walk $tree

################################################################################
# Linked list example

struct llnode val next
struct llist head tail

proc llist-node-add {head val} {
	set n [make-llnode]
	set-llnode.val $n $val
	set-llnode.next $n $head
	return $n
}

proc llist-node-print head {
	if {[isref $head]} {
	 	puts -nonewline "[llnode.val $head] "
		# TCL isn't tail recursive, but it's just for fun
		llist-node-print [llnode.next $head]
	} else {
		puts {}
	}
}

# Now two simple wrappers to abstract a linked list from
# it's nodes. Just a structure that holds the head of the list.

# Add elements into the list. Returns a pointer to the inserted node.
proc llist-add {listref args} {
    foreach v $args {
	set-llist.head $listref [llist-node-add [llist.head $listref] $v]
    }
    if {![isref [llist.tail $listref]]} {
	set-llist.tail $listref [llist.head $listref]
    }
    return $listref
}

# Print the list content
proc llist-print listref {
    llist-node-print [llist.head $listref]
}

# Link the list's tail with the head, making it circular.
proc llist-make-circular listref {
    set head [llist.head $listref]
    set tail [llist.tail $listref]
    set-llnode.next $tail $head
}

# Example of linked list usage:
# create a linked list:

set mylist [make-llist]
for {set i 0} {$i < 10} {incr i} {
    llist-add $mylist $i
}

# Print the list
puts -nonewline "Linked list: "
llist-print $mylist

# Make it circular and walk the ring of objects 20 times
puts -nonewline "Circular linked list: "
llist-make-circular $mylist

# Show it's actually a cycle
set node [llist.head $mylist]
for {set i 0} {$i < 20} {incr i} {
    if {![isref $node]} exit
    puts -nonewline "[llnode.val $node] "
    set node [llnode.next $node]
}
puts {}

# Collect cyclical data structures now. (The collection is done
# automatically when needed.)

if 0 {
    set mylist {}
}

set node {}
set mylist {}

# Now collect cycles. This call is not really useful
# as the collection is automatically performed when
# needed.
collect
