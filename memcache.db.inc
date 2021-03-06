<?php

require_once 'dmemcache.inc';

/** Implementation of cache.inc with memcache logic included **/

/**
 * Return data from the persistent cache.
 *
 * @param $key
 *   The cache ID of the data to retrieve.
 * @param $table
 *   The table $table to store the data in. Valid core values are 'cache_filter',
 *   'cache_menu', 'cache_page', or 'cache' for the default cache.
 */
function cache_get($key, $table = 'cache') {
  global $user;

  // Garbage collection necessary when enforcing a minimum cache lifetime
  $cache_flush = variable_get('cache_flush', 0);
  if ($cache_flush && ($cache_flush + variable_get('cache_lifetime', 0) <= time())) {
    // Time to flush old cache data
    db_query("DELETE FROM {%s} WHERE expire != %d AND expire <= %d", $table, CACHE_PERMANENT, $cache_flush);
    variable_set('cache_flush', 0);
  }

  // If we have a memcache hit for this, return it.
  if ($cache = dmemcache_get($key, $table)) {
    return $cache;
  }

  // Look for a database cache hit.
  if ($cache = db_fetch_object(db_query("SELECT data, created, headers, expire, serialized FROM {%s} WHERE cid = '%s'", $table, $key))) {
    if (isset($cache->data)) {
      // If the data is permanent or we're not enforcing a minimum cache lifetime
      // always return the cached data.
      if ($cache->expire == CACHE_PERMANENT || !variable_get('cache_lifetime', 0)) {
        $cache->data = db_decode_blob($cache->data);
        if ($cache->serialized) {
          $cache->data = unserialize($cache->data);
        }
      }
      // If enforcing a minimum cache lifetime, validate that the data is
      // currently valid for this user before we return it by making sure the
      // cache entry was created before the timestamp in the current session's
      // cache timer. The cache variable is loaded into the $user object by
      // sess_read() in session.inc.
      else {
        if ($user->cache > $cache->created) {
          // This cache data is too old and thus not valid for us, ignore it.
          return 0;
        }
        else {
          $cache->data = db_decode_blob($cache->data);
          if ($cache->serialized) {
            $cache->data = unserialize($cache->data);
          }
        }
      }
    }

    // By calling cache_set with an extra paramater to signify no db storage,
    // we can lazy instantiate memcache that just comes online.
    cache_set($key, $table, $cache->data, $cache->expire, $cache->headers, FALSE);
    return $cache;
  }
  return 0;
}

/**
 * Store data in the persistent cache.
 *
 * The persistent cache is split up into four database
 * tables. Contributed modules can add additional tables.
 *
 * 'cache_page': This table stores generated pages for anonymous
 * users. This is the only table affected by the page cache setting on
 * the administrator panel.
 *
 * 'cache_menu': Stores the cachable part of the users' menus.
 *
 * 'cache_filter': Stores filtered pieces of content. This table is
 * periodically cleared of stale entries by cron.
 *
 * 'cache': Generic cache storage table.
 *
 * The reasons for having several tables are as follows:
 *
 * - smaller tables allow for faster selects and inserts
 * - we try to put fast changing cache items and rather static
 *   ones into different tables. The effect is that only the fast
 *   changing tables will need a lot of writes to disk. The more
 *   static tables will also be better cachable with MySQL's query cache
 *
 * @param $cid
 *   The cache ID of the data to store.
 * @param $table
 *   The table $table to store the data in. Valid core values are 'cache_filter',
 *   'cache_menu', 'cache_page', or 'cache'.
 * @param $data
 *   The data to store in the cache. Complex data types must be serialized first.
 * @param $expire
 *   One of the following values:
 *   - CACHE_PERMANENT: Indicates that the item should never be removed unless
 *     explicitly told to using cache_clear_all() with a cache ID.
 *   - CACHE_TEMPORARY: Indicates that the item should be removed at the next
 *     general cache wipe.
 *   - A Unix timestamp: Indicates that the item should be kept at least until
 *     the given time, after which it behaves like CACHE_TEMPORARY.
 * @param $headers
 *   A string containing HTTP header information for cached pages.
 * @param $db_storage
 *   This boolean is unique to the memcache.inc implementation of cache set.
 *   It allows us to do a cache_set and not write to the database, but only
 *   to memcache.
 */
function cache_set($cid, $table = 'cache', $data, $expire = CACHE_PERMANENT, $headers = NULL, $db_storage = TRUE) {
  $time = time();

  // Create new cache object.
  $cache = new stdClass;
  $cache->cid = $cid;
  $cache->data = is_object($data) ? memcache_clone($data) : $data;
  $cache->created = $time;
  $cache->expire = $expire;
  $cache->headers = $headers;

  if ($db_storage) {
    $serialized = 0;
    if (is_object($data) || is_array($data)) {
      $data = serialize($data);
      $serialized = 1;
    }
    // Save to the database
    db_query("
       INSERT INTO {$table} (data, created, expire, headers, serialized, cid) VALUES (%b, %d, %d, '%s', %d, '%s') ON DUPLICATE KEY
       UPDATE data = %b, created = %d, expire = %d, headers = '%s', serialized = %d",
       $data, $time, $expire, $headers, $serialized, $cid,
       $data, $time, $expire, $headers, $serialized, $cid);
  }

  // Save to memcache
  if ($expire == CACHE_TEMPORARY) {
    $expire = variable_get('cache_lifetime', 2591999);
  }
  dmemcache_set($cid, $cache, $expire, $table);
}

/**
 *
 * Expire data from the cache. If called without arguments, expirable
 * entries will be cleared from the cache_page table.
 *
 * Memcache logic is simpler than the core cache because memcache doesn't have
 * a minimum cache lifetime consideration (it handles it internally), and 
 * doesn't support wildcards.  Wildcard flushes result in the entire table
 * being flushed.
 *
 * @param $cid
 *   If set, the cache ID to delete. Otherwise, all cache entries that can
 *   expire are deleted from the specified table.
 *
 * @param $table
 *   If set, the table delete from.
 *
 * @param $wildcard
 *   If set to TRUE, the $cid is treated as a substring
 *   to match rather than a complete ID. The match is a right hand
 *   match. If '*' is given as $cid, the table $table will be emptied.
 */
function cache_clear_all($cid = NULL, $table = NULL, $wildcard = FALSE) {
  global $user;

  if (!isset($cid) && !isset($table)) {
    $cid = '*';
    $wildcard = TRUE;
    $table = 'cache_page';
  }
  if (empty($cid) || ($cid == '*' && $wildcard !== TRUE)) {
    # don't do anything if cid is unset. this matches the default drupal behavior...
    if ($wildcard && $cid != '*') {
      if (variable_get('memcache_debug', FALSE)) {
        // call watchdog, since you probably didn't want to flush the entire bin.
        watchdog('memcache', "illegal wildcard in cache_clear_all - not flushing entire bin. table: $table. cid: $cid", WATCHDOG_WARNING);
      }
    }
  }
  else if ($cid == '*' || $wildcard === TRUE) {
    dmemcache_flush($table);
  }
  else {
    dmemcache_delete($cid, $table);
  }

  if (empty($cid)) {
    if (variable_get('cache_lifetime', 0)) {
      // We store the time in the current user's $user->cache variable which
      // will be saved into the sessions table by sess_write(). We then
      // simulate that the cache was flushed for this user by not returning
      // cached data that was cached before the timestamp.
      $user->cache = time();

      $cache_flush = variable_get('cache_flush', 0);
      if ($cache_flush == 0) {
        // This is the first request to clear the cache, start a timer.
        variable_set('cache_flush', time());
      }
      else if (time() > ($cache_flush + variable_get('cache_lifetime', 0))) {
        // Clear the cache for everyone, cache_flush_delay seconds have
        // passed since the first request to clear the cache.
        db_query("DELETE FROM {%s} WHERE expire != %d AND expire < %d", $table, CACHE_PERMANENT, time());
        variable_set('cache_flush', 0);
      }
    }
    else {
      // No minimum cache lifetime, flush all temporary cache entries now.
      db_query("DELETE FROM {%s} WHERE expire != %d AND expire < %d", $table, CACHE_PERMANENT, time());
    }
  }
  else {
    if ($wildcard) {
      if ($cid == '*') {
        db_query("DELETE FROM {%s}", $table);
      }
      else {
        db_query("DELETE FROM {%s} WHERE cid LIKE '%s%%'", $table, $cid);
      }
    }
    else {
      db_query("DELETE FROM {%s} WHERE cid = '%s'", $table, $cid);
    }
  }
}

/**
 * Provide a substitute clone() function for PHP4. This is a copy of drupal_clone
 * because common.inc isn't included early enough in the bootstrap process to
 * be able to depend on drupal_clone.
 */
function memcache_clone($object) {
  return version_compare(phpversion(), '5.0') < 0 ? $object : clone($object);
}
