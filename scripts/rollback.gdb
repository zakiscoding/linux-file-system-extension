set pagination off
set confirm off
target remote 127.0.0.1:1234
break do_init_module
ignore 1 1
continue
lx-symbols ../module
delete 1
break edufs_update if fail_alloc_after == 1
continue
echo \n--- Indirect allocation rollback inspection ---\n
bt
print inode->i_ino
print target
print fail_alloc_after
set $saved_inode = inode
print $saved_inode->i_size
finish
print (int)$eax
print $saved_inode->i_size
if (int)$eax != -28
    echo Unexpected rollback result\n
    quit 1
end
if $saved_inode->i_size != 49152
    echo Rollback changed file size\n
    quit 1
end
echo EDUFS GDB ROLLBACK PASS\n
detach
quit
