echo hello world
?
cat < testfile
cat testfile > testfileout
cat < testfile > testfileout
?
cat testfile | grep "goodbye"
false ; echo "should print 1"
?
false && echo "should not print 1"
false || echo "should print 2"
?
true || echo "should not print 2"
false
?