This is a fork of the Drupal contributed memcache project
http://drupal.org/project/memcache

This fork only exists to add one feature, which we do not think belongs in the
main project. This fork allows two or more Drupal installs to share the same
database with different memcache pulls. This can be used for the following
situations:

 - a 'master' Drupal install, with a read-only install elsewhere, for example
   a separate datacentre. Cache writes from the master should not be written
   across the network, but cache invalidation should so that the read-only site
   stays in sync. This assumes a read-only MySQL install that replicates from the
   master.

 - a multi-site install with a shared database and some prefixed tables. Some cache
  entries may differ due to the tables that are prefixed, but since some tables are
  shared, operations that affect those tables should clear the cache on both sites.
  This fork allows for separate memcache instances but with cache clears affecting
  both.
 
