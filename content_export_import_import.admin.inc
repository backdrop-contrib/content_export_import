<?php

/**
 * @file
 * Import form to import content from another Backdrop or WordPress site.
 */

/**
 * Form callback: Provides the content import form.
 *
 * @param array $form
 *   The form array.
 * @param array $form_state
 *   The form state array.
 *
 * @return array
 *   The complete form array.
 */
function content_export_import_import_settings($form, &$form_state) {
  $form = array();
  $form['import'] = array(
    '#type' => 'fieldset',
    '#title' => t('Import Content'),
  );
  // @todo Implement auto-detection of import source type.
  if (!empty($form_state['storage']['import-file'])) {
    $ready_file = $form_state['storage']['import-file'];
    $form['import']['loaded_file'] = array(
      '#type' => 'markup',
      '#markup' => '<p class="description">' . t('File ready: %filename — choose a new file below to replace it.', array('%filename' => $ready_file->filename)) . '</p>',
    );
  }
  $form['import']['import-file'] = array(
    '#type' => 'file',
    '#title' => t('Upload a ZIP file'),
    '#description' => t('Upload a ZIP file containing content export from a Backdrop site.'),
    '#upload_validators' => array(
      'file_validate_extensions' => array('zip'),
      'file_validate_size' => array(file_upload_max_size()),
    ),
  );
  $form['import']['preview_summary'] = array(
    '#type' => 'submit',
    '#value' => t('Preview Summary'),
    '#attributes' => array('class' => array('button')),
    '#submit' => array('content_export_import_import_settings_submit'),
  );
  $form['import']['preview_detailed'] = array(
    '#type' => 'submit',
    '#value' => t('Preview Detailed'),
    '#attributes' => array('class' => array('button')),
    '#submit' => array('content_export_import_import_settings_submit'),
  );
  $form['import']['submit'] = array(
    '#type' => 'submit',
    '#value' => t('Import Content'),
    '#attributes' => array(
      'class' => array('button-primary'),
    ),
    '#submit' => array('content_export_import_import_settings_submit'),
  );

  return $form;
}

/**
 * Validate handler for the content import form.
 *
 * @param array $form
 *   The form array.
 * @param array $form_state
 *   The form state array.
 */
function content_export_import_import_settings_validate($form, &$form_state) {
  // If a file was retained from a previous dry run, reuse it.
  if (!empty($form_state['storage']['import-file'])) {
    return;
  }

  $validators = array(
    'file_validate_extensions' => array('zip'),
    'file_validate_size' => array(file_upload_max_size()),
  );

  $file = file_save_upload('import-file', $validators);

  if ($file) {
    if ($file = file_move($file, 'public://')) {
      $form_state['storage']['import-file'] = $file;
    }
    else {
      form_set_error(
        'import-file',
        t("Failed to write the uploaded file to the site's file folder.")
      );
    }
  }
  else {
    form_set_error('import-file', t('The import could not be completed.'));
  }
}

/**
 * Submit handler for the content_export_import_import_settings form.
 */
function content_export_import_import_settings_submit(&$form, &$form_state) {
  // Detect dry run mode first so we know whether to keep the file in storage.
  $triggered = $form_state['triggering_element']['#value'];
  $dry_run  = ($triggered === t('Preview Summary') || $triggered === t('Preview Detailed'));
  $detailed = ($triggered === t('Preview Detailed'));

  $file = $form_state['storage']['import-file'];
  // Only discard the stored file after an actual import, not after a dry run.
  if (!$dry_run) {
    unset($form_state['storage']['import-file']);
  }
  
  // Validate file exists and is readable
  if (!isset($file->uri) || !file_exists($file->uri)) {
    backdrop_set_message(t('Error: Import file not found or not readable.'), 'error');
    return;
  }

  // Create a temporary directory for extraction
  $temp_dir = 'temporary://content_export_import_import_' . time();
  file_prepare_directory($temp_dir, FILE_CREATE_DIRECTORY);

  // Extract the zip file
  $zip = new ZipArchive();
  if ($zip->open(backdrop_realpath($file->uri)) === TRUE) {
    $zip->extractTo(backdrop_realpath($temp_dir));
    $zip->close();
  }
  else {
    backdrop_set_message(t('Error: Could not extract the ZIP file.'), 'error');
    return;
  }

  // Read the JSON file
  $json_file = $temp_dir . '/content.json';
  if (!file_exists($json_file)) {
    backdrop_set_message(t('Error: content.json not found in the ZIP file.'), 'error');
    return;
  }

  $content = file_get_contents($json_file);
  if ($content === FALSE) {
    backdrop_set_message(t('Error: Could not read the content.json file.'), 'error');
    return;
  }

  $content = backdrop_json_decode($content);
  if ($content === NULL) {
    backdrop_set_message(t('Error: The content.json file is not valid JSON.'), 'error');
    return;
  }

  $report = array(
    'vocabularies_created' => array(),
    'vocabularies_existing' => array(),
    'terms_created'  => array(),
    'terms_existing' => array(),
    'types_created'  => array(),
    'types_existing' => array(),
    'nodes_to_import'       => array(),
    'nodes_skipped_uuid'    => array(),
    'nodes_duplicate_title' => array(),
    'files_to_import' => 0,
    'comments_to_import' => 0,
  );

  // Process files directory if it exists
  $files_dir = $temp_dir . '/files';
  if (file_exists($files_dir)) {
    $files = file_scan_directory($files_dir, '/.*/');
    foreach ($files as $file) {
      // Ensure file URI exists and is not null
      if (!isset($file->uri) || empty($file->uri)) {
        backdrop_set_message(t('Warning: Skipping file with invalid URI.'), 'warning');
        continue;
      }

      // Extract file ID from filename (format: fid_filename)
      $basename = basename($file->uri);
      if (empty($basename)) {
        backdrop_set_message(t('Warning: Skipping file with invalid basename.'), 'warning');
        continue;
      }

      $parts = explode('_', $basename, 2);
      if (count($parts) === 2) {
        $fid = $parts[0];
        $filename = $parts[1];
        
        // Validate filename
        if (empty($filename)) {
          backdrop_set_message(t('Warning: Skipping file with empty filename.'), 'warning');
          continue;
        }

        // Check if we already have a mapping for this file ID
        if (isset($form_state['storage']['file_map'][$fid])) {
          continue;
        }

        // Check if file with this name already exists
        $existing_files = file_load_multiple(array(), array('filename' => $filename));
        if (!empty($existing_files)) {
          // Use the first matching file
          $existing_file = reset($existing_files);
          if (!$dry_run) {
            $form_state['storage']['file_map'][$fid] = $existing_file->fid;
          }
          continue;
        }

        // Count this file as one that would be imported.
        $report['files_to_import']++;

        if (!$dry_run) {
          // Create a new file entry
          $new_file = new File();
          // Generate a unique filename by adding a timestamp
          $unique_filename = time() . '_' . $filename;
          $new_file->uri = 'public://' . $unique_filename;
          $new_file->filename = $filename;
          $new_file->filemime = file_get_mimetype($filename);
          $new_file->uid = $GLOBALS['user']->uid;
          $new_file->status = FILE_STATUS_PERMANENT;

          // Ensure source file exists and is readable
          if (!file_exists($file->uri) || !is_readable($file->uri)) {
            backdrop_set_message(t('Warning: Source file @file does not exist or is not readable.', array('@file' => $file->uri)), 'warning');
            continue;
          }

          // Ensure destination directory exists
          $dest_dir = dirname(backdrop_realpath($new_file->uri));
          if (!file_exists($dest_dir)) {
            file_prepare_directory($dest_dir, FILE_CREATE_DIRECTORY);
          }

          // Copy the file to the public directory
          if (copy($file->uri, backdrop_realpath($new_file->uri))) {
            file_save($new_file);
            // Store the mapping of old FID to new FID
            $form_state['storage']['file_map'][$fid] = $new_file->fid;
          }
          else {
            backdrop_set_message(t('Warning: Failed to copy file @file.', array('@file' => $filename)), 'warning');
          }
        }
      }
    }
  }

  // Keep a reference to the full content data for dry run type validation.
  $content_data = $content;
  // Check if this is a new format export with content types
  $comments_to_import = array();
  if (isset($content['content_types']) && isset($content['nodes'])) {
    // Store comments data for later import (after nodes are imported)
    if (isset($content['comments']) && is_array($content['comments'])) {
      $comments_to_import = $content['comments'];
      // Enable the comment module if comments are present and it is not active.
      if (!$dry_run && !empty($comments_to_import) && !module_exists('comment')) {
        module_enable(array('comment'));
        backdrop_set_message(t('Enabled the Comment module (required for importing comments).'));
      }
    }
    
    // First import taxonomy vocabularies and terms
    if (isset($content['taxonomy'])) {
      // Create a mapping of old term IDs to new term IDs
      $term_id_map = array();
      
      // Import vocabularies first
      if (isset($content['taxonomy']['vocabularies'])) {
        foreach ($content['taxonomy']['vocabularies'] as $machine_name => $vocab_data) {
          // Check if vocabulary exists
          $existing_vocab = taxonomy_vocabulary_machine_name_load($machine_name);
          if (!$existing_vocab) {
            $report['vocabularies_created'][] = $vocab_data['name'];
            if (!$dry_run) {
              // Create new vocabulary using the proper class
              $vocabulary = new TaxonomyVocabulary();
              $vocabulary->name = $vocab_data['name'];
              $vocabulary->machine_name = $vocab_data['machine_name'];
              $vocabulary->description = $vocab_data['description'];
              $vocabulary->hierarchy = $vocab_data['hierarchy'];
              $vocabulary->weight = $vocab_data['weight'];
              if (isset($vocab_data['language'])) {
                $vocabulary->language = $vocab_data['language'];
              }
              taxonomy_vocabulary_save($vocabulary);
              backdrop_set_message(t('Created new vocabulary: @name', array('@name' => $vocabulary->name)));
            }
          }
          else {
            $report['vocabularies_existing'][] = $vocab_data['name'];
          }
        }
      }

      // Import terms
      if (isset($content['taxonomy']['terms'])) {
        // Create a mapping of old term IDs to new term IDs
        $term_id_map = array();
        $term_count = 0;
        $term_skipped_count = 0;

        // First pass: Create all terms without parents
        foreach ($content['taxonomy']['terms'] as $vocabulary_machine_name => $terms) {
          foreach ($terms as $old_tid => $term_data) {
            if (!isset($term_data['parent']) || $term_data['parent'] == 0) {
              $vocabulary = taxonomy_vocabulary_machine_name_load($vocabulary_machine_name);
              if ($vocabulary) {
                // Check if a term with this name already exists in the vocabulary.
                $existing = taxonomy_term_load_multiple(array(), array(
                  'name' => $term_data['name'],
                  'vocabulary' => $vocabulary_machine_name,
                ));
                if (!empty($existing)) {
                  $existing_term = reset($existing);
                  $term_id_map[$old_tid] = $existing_term->tid;
                  $term_skipped_count++;
                  if (!isset($report['terms_existing'][$vocabulary_machine_name])) {
                    $report['terms_existing'][$vocabulary_machine_name] = array();
                  }
                  $report['terms_existing'][$vocabulary_machine_name][] = $term_data['name'];
                  continue;
                }

                if (!isset($report['terms_created'][$vocabulary_machine_name])) {
                  $report['terms_created'][$vocabulary_machine_name] = array();
                }
                $report['terms_created'][$vocabulary_machine_name][] = $term_data['name'];

                if (!$dry_run) {
                  $term = new TaxonomyTerm();
                  $term->name = $term_data['name'];
                  $term->description = $term_data['description'];
                  $term->format = $term_data['format'];
                  $term->weight = $term_data['weight'];
                  $term->vocabulary = $vocabulary->machine_name;
                  $term->parent = array(0);
                  if (isset($term_data['language'])) {
                    $term->language = $term_data['language'];
                  }
                  taxonomy_term_save($term);
                  $term_id_map[$old_tid] = $term->tid;
                }
                else {
                  // Use a placeholder TID so child terms can reference it.
                  $term_id_map[$old_tid] = 'dry_run_' . $old_tid;
                }
                $term_count++;
              }
            }
          }
        }

        // Second pass: Create terms with parents
        foreach ($content['taxonomy']['terms'] as $vocabulary_machine_name => $terms) {
          foreach ($terms as $old_tid => $term_data) {
            if (isset($term_data['parent']) && $term_data['parent'] != 0 && !isset($term_id_map[$old_tid])) {
              $vocabulary = taxonomy_vocabulary_machine_name_load($vocabulary_machine_name);
              if ($vocabulary) {
                // Check if a term with this name already exists in the vocabulary.
                $existing = taxonomy_term_load_multiple(array(), array(
                  'name' => $term_data['name'],
                  'vocabulary' => $vocabulary_machine_name,
                ));
                if (!empty($existing)) {
                  $existing_term = reset($existing);
                  $term_id_map[$old_tid] = $existing_term->tid;
                  $term_skipped_count++;
                  if (!isset($report['terms_existing'][$vocabulary_machine_name])) {
                    $report['terms_existing'][$vocabulary_machine_name] = array();
                  }
                  $report['terms_existing'][$vocabulary_machine_name][] = $term_data['name'];
                  continue;
                }

                if (!isset($report['terms_created'][$vocabulary_machine_name])) {
                  $report['terms_created'][$vocabulary_machine_name] = array();
                }
                $report['terms_created'][$vocabulary_machine_name][] = $term_data['name'];

                if (!$dry_run) {
                  $term = new TaxonomyTerm();
                  $term->name = $term_data['name'];
                  $term->description = $term_data['description'];
                  $term->format = $term_data['format'];
                  $term->weight = $term_data['weight'];
                  $term->vocabulary = $vocabulary->machine_name;
                  $term->parent = array(isset($term_id_map[$term_data['parent']]) ? $term_id_map[$term_data['parent']] : 0);
                  if (isset($term_data['language'])) {
                    $term->language = $term_data['language'];
                  }
                  taxonomy_term_save($term);
                  $term_id_map[$old_tid] = $term->tid;
                }
                else {
                  $term_id_map[$old_tid] = 'dry_run_' . $old_tid;
                }
                $term_count++;
              }
            }
          }
        }

        if (!$dry_run) {
          if ($term_count > 0) {
            backdrop_set_message(t('Created @count new taxonomy terms.', array('@count' => $term_count)));
          }
          if ($term_skipped_count > 0) {
            backdrop_set_message(t('@count taxonomy terms were not imported because they already exist.', array('@count' => $term_skipped_count)));
          }
        }
      }
    }

    // Import content types first
    foreach ($content['content_types'] as $type => $type_data) {
      // Check if content type exists
      if (!node_type_get_type($type)) {
        $report['types_created'][] = isset($type_data['name']) ? $type_data['name'] : $type;

        if (!$dry_run) {
          // Create the content type
          $content_type = new stdClass();
          foreach ($type_data as $key => $value) {
            if ($key !== 'field_instances' && $key !== 'field_configs') {
              $content_type->$key = $value;
            }
          }
          node_type_save($content_type);

          // Inform about the new content type creation
          backdrop_set_message(t('Created new content type: @name (@type)', array(
            '@name' => $content_type->name,
            '@type' => $content_type->type
          )));

          // Grant admin role all permissions for this content type
          $admin_rid = config_get('system.core', 'user_admin_role');
          if ($admin_rid) {
            $permissions = array(
              "create {$type} content" => TRUE,
              "edit any {$type} content" => TRUE,
              "edit own {$type} content" => TRUE,
              "delete any {$type} content" => TRUE,
              "delete own {$type} content" => TRUE,
            );
            user_role_change_permissions($admin_rid, $permissions);
            backdrop_set_message(t('Granted administrator permissions for content type: @type', array('@type' => $content_type->name)));
          }

          // Import field configurations
          if (isset($type_data['field_configs'])) {
            foreach ($type_data['field_configs'] as $field_name => $field_config) {
              if (!field_info_field($field_name)) {
                // Enable the field's module if it is not already available.
                if (!empty($field_config['module']) && !module_exists($field_config['module'])) {
                  $enabled = module_enable(array($field_config['module']));
                  if ($enabled) {
                    // Clear field info caches so the new field type is available.
                    field_cache_clear();
                    backdrop_set_message(t('Enabled the %module module (required by field %field).', array(
                      '%module' => $field_config['module'],
                      '%field' => $field_name,
                    )));
                  }
                  else {
                    backdrop_set_message(t('Could not enable the %module module required by field %field. Skipping field.', array(
                      '%module' => $field_config['module'],
                      '%field' => $field_name,
                    )), 'error');
                    continue;
                  }
                }
                field_create_field($field_config);
              }
            }
          }

          // Add UUID field if it doesn't exist
          if (!field_info_field('field_uuid')) {
            $uuid_field = array(
              'field_name' => 'field_uuid',
              'type' => 'text',
              'cardinality' => 1,
              'locked' => TRUE,
              'settings' => array(
                'max_length' => 36,
              ),
            );
            field_create_field($uuid_field);
          }

          // Import field instances
          if (isset($type_data['field_instances'])) {
            foreach ($type_data['field_instances'] as $field_name => $instance) {
              if (!field_info_instance('node', $field_name, $type)) {
                field_create_instance($instance);
              }
            }
          }

          // Add UUID field instance if it doesn't exist
          if (!field_info_instance('node', 'field_uuid', $type)) {
            $uuid_instance = array(
              'field_name' => 'field_uuid',
              'entity_type' => 'node',
              'bundle' => $type,
              'label' => 'UUID',
              'description' => 'Unique identifier for this content',
              'required' => FALSE,
              'widget' => array(
                'type' => 'text_textfield',
                'settings' => array(
                  'size' => 60,
                ),
              ),
              'display' => array(
                'default' => array(
                  'label' => 'above',
                  'type' => 'text_default',
                ),
              ),
            );
            field_create_instance($uuid_instance);
          }

          // Ensure comment_body field is created for this content type (if comment module is enabled)
          // This is normally done by hook_node_type_insert(), but we need to do it manually
          if (module_exists('comment') && function_exists('_comment_body_field_create')) {
            // Reload the content type to ensure it's fully saved
            $content_type = node_type_get_type($type);
            if ($content_type) {
              _comment_body_field_create($content_type);
            }
          }

          // Import comment settings for this content type (if comment module is enabled)
          if (module_exists('comment') && isset($type_data['comment_settings']) && is_array($type_data['comment_settings'])) {
            // Reload the content type to get the full object with settings
            $content_type = node_type_get_type($type);
            if ($content_type) {
              // Initialize settings array if it doesn't exist
              if (!isset($content_type->settings) || !is_array($content_type->settings)) {
                $content_type->settings = array();
              }
              // Merge comment settings
              $content_type->settings = array_merge($content_type->settings, $type_data['comment_settings']);
              node_type_save($content_type);
              backdrop_set_message(t('Imported comment settings for content type: @type', array('@type' => $content_type->name)));
            }
          }

          // Clear the menu cache to make the new content type available
          menu_rebuild();
        }
      }
      else {
        // Content type already exists.
        $report['types_existing'][] = isset($type_data['name']) ? $type_data['name'] : $type;

        if (!$dry_run) {
          // Ensure field_uuid field exists.
          if (!field_info_field('field_uuid')) {
            $uuid_field = array(
              'field_name' => 'field_uuid',
              'type' => 'text',
              'cardinality' => 1,
              'locked' => TRUE,
              'settings' => array(
                'max_length' => 36,
              ),
            );
            field_create_field($uuid_field);
          }

          // Ensure field_uuid instance exists for this content type.
          if (!field_info_instance('node', 'field_uuid', $type)) {
            $uuid_instance = array(
              'field_name' => 'field_uuid',
              'entity_type' => 'node',
              'bundle' => $type,
              'label' => 'UUID',
              'description' => 'Unique identifier for this content',
              'required' => FALSE,
              'widget' => array(
                'type' => 'text_textfield',
                'settings' => array(
                  'size' => 60,
                ),
              ),
              'display' => array(
                'default' => array(
                  'label' => 'above',
                  'type' => 'text_default',
                ),
              ),
            );
            field_create_instance($uuid_instance);
          }

          if (module_exists('comment')) {
            $content_type = node_type_get_type($type);
            if ($content_type && function_exists('_comment_body_field_create')) {
              // Ensure comment_body field is created for this content type
              _comment_body_field_create($content_type);
            }

            // Import comment settings if they exist
            if (isset($type_data['comment_settings']) && is_array($type_data['comment_settings'])) {
              $content_type = node_type_get_type($type);
              if ($content_type) {
                // Initialize settings array if it doesn't exist
                if (!isset($content_type->settings) || !is_array($content_type->settings)) {
                  $content_type->settings = array();
                }
                // Merge comment settings
                $content_type->settings = array_merge($content_type->settings, $type_data['comment_settings']);
                node_type_save($content_type);
                backdrop_set_message(t('Updated comment settings for content type: @type', array('@type' => $content_type->name)));
              }
            }
          }
        }
      }
    }

    // Now import the nodes
    $content = $content['nodes'];
  }

  // First pass: Create a mapping of UUIDs to new NIDs
  $uuid_to_nid = array();
  foreach ($content as $c) {
    if (isset($c['uuid'])) {
      $uuid_to_nid[$c['uuid']] = NULL; // Will be populated after node creation
    }
  }

  // Second pass: Import nodes and update the mapping
  $node_count = 0;
  $skipped_count = 0;
  $duplicate_titles = array();
  foreach ($content as $c) {
    if(isset($c['nid'])) {
      unset($c['nid']);
    }
    if (isset($c['vid'])) {
      unset($c['vid']);
    }

    // Check if node with this UUID already exists
    if (isset($c['uuid']) && content_export_import_entity_exists_by_uuid('node', $c['uuid'])) {
      $skipped_count++;
      $report['nodes_skipped_uuid'][] = isset($c['title']) ? $c['title'] : $c['uuid'];
      continue;
    }

    // Validate that the content type exists or would be created by this import.
    $type_name = isset($c['type']) ? $c['type'] : '';
    $type_would_be_created = isset($content_data['content_types'][$type_name]);
    if (empty($type_name) || (!node_type_get_type($type_name) && !$type_would_be_created)) {
      backdrop_set_message(t('Skipping node Test: Content type @type does not exist.', array('@type' => $type_name ? $type_name : 'undefined')), 'error');
      continue;
    }

    // Check if a node with the same title and type already exists. The node
    // will still be imported, but the user will be warned.
    if (!empty($c['title'])) {
      $existing_title = db_select('node', 'n')
        ->fields('n', array('nid'))
        ->condition('n.title', $c['title'])
        ->condition('n.type', $c['type'])
        ->range(0, 1)
        ->execute()
        ->fetchField();
      if ($existing_title) {
        $duplicate_titles[] = check_plain($c['title']);
        $report['nodes_duplicate_title'][] = $c['title'];
      }
    }

    // During dry run, skip node building — just track for the report.
    if ($dry_run) {
      $report['nodes_to_import'][] = isset($c['title']) ? $c['title'] : '';
      $node_count++;
      if (isset($c['uuid'])) {
        $uuid_to_nid[$c['uuid']] = 'dry_run';
      }
      continue;
    }

    // Create a new node using the proper Node class
    $node = entity_create('node', array(
      'type' => $c['type'],
      'title' => isset($c['title']) ? $c['title'] : '',
      'language' => LANGUAGE_NONE,
      'uid' => $GLOBALS['user']->uid,
      'status' => 1,
      'path' => array(
        'alias' => isset($c['path']['alias']) ? $c['path']['alias'] : '',
      ),
    ));

    // Get the content type settings
    $content_type = node_type_get_type($node->type);
    if (!$content_type) {
      backdrop_set_message(t('Skipping node: Could not load content type settings for @type', array('@type' => $node->type)), 'error');
      continue;
    }
    
    // Copy all other properties from the imported content
    foreach ($c as $key => $value) {
      if (!in_array($key, array('nid', 'vid', 'type', 'title', 'language', 'uid', 'status', 'path'))) {
        // Handle file fields specially
        if (strpos($key, 'field_') === 0) {
          $field_info = field_info_field($key);
          if ($field_info && ($field_info['type'] === 'file' || $field_info['type'] === 'image')) {
            // Skip if value is null or empty
            if (empty($value)) {
              continue;
            }

            // Update file IDs using our mapping
            if (is_array($value)) {
              $valid_items = array();
              foreach ($value as $lang => $items) {
                if (is_array($items)) {
                  $valid_items[$lang] = array();
                  foreach ($items as $delta => $item) {
                    // Skip if item is null or doesn't have required properties
                    if (empty($item) || !isset($item['fid'])) {
                      continue;
                    }

                    // Skip if we don't have a mapping for this file
                    if (!isset($form_state['storage']['file_map'][$item['fid']])) {
                      if (!$dry_run) {
                        backdrop_set_message(t('Warning: File ID @fid in field @field was not included in the import.', array('@fid' => $item['fid'], '@field' => $key)), 'warning');
                      }
                      continue;
                    }

                    // Create a new file item with only the necessary properties
                    $new_item = array(
                      'fid' => $form_state['storage']['file_map'][$item['fid']],
                      'display' => isset($item['display']) ? $item['display'] : 1,
                      'description' => isset($item['description']) ? $item['description'] : '',
                    );

                    // Only add URI if we have a valid file
                    if (isset($item['uri']) && !empty($item['uri'])) {
                      $new_item['uri'] = $item['uri'];
                    }

                    $valid_items[$lang][] = $new_item;
                  }
                }
              }
              // Only set the field value if we have valid items
              if (!empty($valid_items)) {
                $value = $valid_items;
              } else {
                continue; // Skip this field entirely if no valid items
              }
            }
          }
          // Handle node reference fields
          elseif ($field_info && $field_info['type'] === 'node_reference') {
            if (is_array($value)) {
              $valid_items = array();
              foreach ($value as $lang => $items) {
                if (is_array($items)) {
                  $valid_items[$lang] = array();
                  foreach ($items as $delta => $item) {
                    if (isset($item['nid'])) {
                      // Find the original node to get its UUID
                      $original_node = NULL;
                      foreach ($content as $original) {
                        if (isset($original['nid']) && $original['nid'] == $item['nid']) {
                          $original_node = $original;
                          break;
                        }
                      }
                      
                      if ($original_node && isset($original_node['uuid']) && isset($uuid_to_nid[$original_node['uuid']])) {
                        $valid_items[$lang][] = array('nid' => $uuid_to_nid[$original_node['uuid']]);
                      }
                    }
                  }
                }
              }
              if (!empty($valid_items)) {
                $value = $valid_items;
              } else {
                continue;
              }
            }
          }
          // Handle taxonomy term reference fields
          elseif ($field_info && $field_info['type'] === 'taxonomy_term_reference') {
            if (is_array($value)) {
              $valid_items = array();
              foreach ($value as $lang => $items) {
                if (is_array($items)) {
                  $valid_items[$lang] = array();
                  foreach ($items as $delta => $item) {
                    if (isset($item['tid'])) {
                      // Update term ID using our mapping
                      if (isset($term_id_map[$item['tid']])) {
                        $valid_items[$lang][] = array('tid' => $term_id_map[$item['tid']]);
                      }
                    }
                  }
                }
              }
              if (!empty($valid_items)) {
                $value = $valid_items;
              } else {
                continue;
              }
            }
          }
        }
        $node->$key = $value;
      }
    }
    
    // Ensure required fields are present
    $required_fields = field_info_instances('node', $node->type);
    $missing_fields = array();
    foreach ($required_fields as $field_name => $instance) {
      if ($instance['required'] && (!isset($node->$field_name) || empty($node->$field_name))) {
        $missing_fields[] = $field_name;
      }
    }
    
    if (!empty($missing_fields)) {
      backdrop_set_message(t('Skipping node: Missing required fields: @fields', array('@fields' => implode(', ', $missing_fields))), 'error');
      continue;
    }

    // Track this node as one that would be imported.
    $report['nodes_to_import'][] = isset($c['title']) ? $c['title'] : '';

    try {
      $node->save();
      $node_count++;
      // Update the UUID to NID mapping
      if (isset($c['uuid'])) {
        $uuid_to_nid[$c['uuid']] = $node->nid;
        // Store the UUID in the field
        $node->field_uuid[LANGUAGE_NONE][0]['value'] = $c['uuid'];
        $node->save();
      }
    }
    catch (Exception $e) {
      backdrop_set_message(t('Error saving node: @error', array('@error' => $e->getMessage())), 'error');
    }
  }

  // Warn about nodes that were imported despite a node with the same title existing.
  if (!$dry_run && !empty($duplicate_titles)) {
    backdrop_set_message(t('The following @count node(s) were imported, but a node with the same title already exists: !titles', array(
      '@count' => count($duplicate_titles),
      '!titles' => '<ul><li>' . implode('</li><li>', $duplicate_titles) . '</li></ul>',
    )), 'warning');
  }

  // Import comments after nodes are imported (if comment module is enabled)
  $comment_count = 0;
  $comment_skipped_count = 0;
  if (module_exists('comment') && !empty($comments_to_import)) {
    if ($dry_run) {
      // In dry run mode, simply count comments for nodes that would be imported.
      foreach ($comments_to_import as $node_uuid => $comments) {
        if (isset($uuid_to_nid[$node_uuid]) && $uuid_to_nid[$node_uuid]) {
          $count = count($comments);
          $report['comments_to_import'] += $count;
          $comment_count += $count;
        }
      }
    }
    else {
      // Create a mapping of comment UUIDs to new CIDs
      $comment_uuid_to_cid = array();
      // Create a mapping of old CIDs to new CIDs (for parent relationships)
      $old_cid_to_new_cid = array();

      // First pass: Import top-level comments (those without parents or with parent = 0)
      foreach ($comments_to_import as $node_uuid => $comments) {
        // Find the new node ID for this UUID
        if (!isset($uuid_to_nid[$node_uuid]) || !$uuid_to_nid[$node_uuid]) {
          continue; // Skip if node wasn't imported
        }
        $new_nid = $uuid_to_nid[$node_uuid];

        foreach ($comments as $comment_data) {
          // Skip if comment already exists (by UUID)
          if (isset($comment_data['uuid']) && isset($comment_uuid_to_cid[$comment_data['uuid']])) {
            $comment_skipped_count++;
            continue;
          }

          // Only import top-level comments in first pass
          if (isset($comment_data['pid']) && $comment_data['pid'] != 0) {
            continue;
          }

          // Create a new comment
          $comment = entity_create('comment', array(
            'nid' => $new_nid,
            'pid' => 0,
            'subject' => isset($comment_data['subject']) ? $comment_data['subject'] : '',
            'uid' => isset($comment_data['uid']) ? $comment_data['uid'] : $GLOBALS['user']->uid,
            'name' => isset($comment_data['name']) ? $comment_data['name'] : '',
            'mail' => isset($comment_data['mail']) ? $comment_data['mail'] : '',
            'homepage' => isset($comment_data['homepage']) ? $comment_data['homepage'] : '',
            'status' => isset($comment_data['status']) ? $comment_data['status'] : COMMENT_PUBLISHED,
            'created' => isset($comment_data['created']) ? $comment_data['created'] : REQUEST_TIME,
            'changed' => isset($comment_data['changed']) ? $comment_data['changed'] : REQUEST_TIME,
          ));

          // Copy other properties, especially comment_body field
          foreach ($comment_data as $key => $value) {
            if (!in_array($key, array('cid', 'nid', 'pid', 'uuid'))) {
              // Handle comment body field - it's a field, so copy it directly
              if ($key === 'comment_body') {
                $comment->$key = $value;
              }
              // Handle other field data (arrays with language keys)
              elseif (is_array($value) && (isset($value[LANGUAGE_NONE]) || isset($value['und']))) {
                $comment->$key = $value;
              }
              // Skip system properties that are already set
              elseif (!in_array($key, array('subject', 'uid', 'name', 'mail', 'homepage', 'status', 'created', 'changed', 'thread', 'node_type', 'registered_name'))) {
                $comment->$key = $value;
              }
            }
          }

          try {
            $comment->save();
            $comment_count++;

            // Store mappings
            if (isset($comment_data['uuid'])) {
              $comment_uuid_to_cid[$comment_data['uuid']] = $comment->cid;
            }
            if (isset($comment_data['cid'])) {
              $old_cid_to_new_cid[$comment_data['cid']] = $comment->cid;
            }
          }
          catch (Exception $e) {
            backdrop_set_message(t('Error saving comment: @error', array('@error' => $e->getMessage())), 'error');
          }
        }
      }

      // Second pass: Import replies (comments with parents)
      // We need to iterate multiple times to handle nested replies
      $max_iterations = 10; // Prevent infinite loops
      $iteration = 0;
      $comments_remaining = TRUE;

      while ($comments_remaining && $iteration < $max_iterations) {
        $iteration++;
        $comments_remaining = FALSE;

        foreach ($comments_to_import as $node_uuid => $comments) {
          // Find the new node ID for this UUID
          if (!isset($uuid_to_nid[$node_uuid]) || !$uuid_to_nid[$node_uuid]) {
            continue;
          }
          $new_nid = $uuid_to_nid[$node_uuid];

          foreach ($comments as $comment_data) {
            // Skip if already imported
            if (isset($comment_data['uuid']) && isset($comment_uuid_to_cid[$comment_data['uuid']])) {
              continue;
            }

            // Skip if no parent or parent is 0 (already handled in first pass)
            if (!isset($comment_data['pid']) || $comment_data['pid'] == 0) {
              continue;
            }

            // Check if parent comment has been imported
            $new_pid = 0;
            if (isset($old_cid_to_new_cid[$comment_data['pid']])) {
              $new_pid = $old_cid_to_new_cid[$comment_data['pid']];
            }
            else {
              // Parent not yet imported, skip for now
              $comments_remaining = TRUE;
              continue;
            }

            // Create a new comment
            $comment = entity_create('comment', array(
              'nid' => $new_nid,
              'pid' => $new_pid,
              'subject' => isset($comment_data['subject']) ? $comment_data['subject'] : '',
              'uid' => isset($comment_data['uid']) ? $comment_data['uid'] : $GLOBALS['user']->uid,
              'name' => isset($comment_data['name']) ? $comment_data['name'] : '',
              'mail' => isset($comment_data['mail']) ? $comment_data['mail'] : '',
              'homepage' => isset($comment_data['homepage']) ? $comment_data['homepage'] : '',
              'status' => isset($comment_data['status']) ? $comment_data['status'] : COMMENT_PUBLISHED,
              'created' => isset($comment_data['created']) ? $comment_data['created'] : REQUEST_TIME,
              'changed' => isset($comment_data['changed']) ? $comment_data['changed'] : REQUEST_TIME,
            ));

            // Copy other properties, especially comment_body field
            foreach ($comment_data as $key => $value) {
              if (!in_array($key, array('cid', 'nid', 'pid', 'uuid'))) {
                // Handle comment body field - it's a field, so copy it directly
                if ($key === 'comment_body') {
                  $comment->$key = $value;
                }
                // Handle other field data (arrays with language keys)
                elseif (is_array($value) && (isset($value[LANGUAGE_NONE]) || isset($value['und']))) {
                  $comment->$key = $value;
                }
                // Skip system properties that are already set
                elseif (!in_array($key, array('subject', 'uid', 'name', 'mail', 'homepage', 'status', 'created', 'changed', 'thread', 'node_type', 'registered_name'))) {
                  $comment->$key = $value;
                }
              }
            }

            try {
              $comment->save();
              $comment_count++;

              // Store mappings
              if (isset($comment_data['uuid'])) {
                $comment_uuid_to_cid[$comment_data['uuid']] = $comment->cid;
              }
              if (isset($comment_data['cid'])) {
                $old_cid_to_new_cid[$comment_data['cid']] = $comment->cid;
              }
            }
            catch (Exception $e) {
              backdrop_set_message(t('Error saving comment: @error', array('@error' => $e->getMessage())), 'error');
            }
          }
        }
      }

      if ($comment_count > 0) {
        backdrop_set_message(t('Imported @count comments.', array('@count' => $comment_count)));
      }
    }
  }

  // Clean up temporary directory
  file_unmanaged_delete_recursive($temp_dir);

  if ($dry_run) {
    _content_export_import_show_report($report, $detailed);
    backdrop_set_message(t('This was a preview only. No changes were made.'));
    // Rebuild the form so the stored file remains available for the next action.
    $form_state['rebuild'] = TRUE;
  }
  else {
    // Give a message to the user.
    $message = t(
      'Import completed successfully. @node_count new nodes were imported (@skipped_count skipped as duplicates) with @file_count associated files.',
      array(
        '@node_count' => $node_count,
        '@skipped_count' => $skipped_count,
        '@file_count' => count($form_state['storage']['file_map']),
      )
    );
    if (isset($comment_count) && $comment_count > 0) {
      $message .= ' ' . t('@comment_count comments were imported.', array('@comment_count' => $comment_count));
    }
    $message .= ' ' . t('Visit the <a href="@content_url">content listing</a> to view the imported content.', array('@content_url' => url('admin/content')));
    backdrop_set_message($message);
  }
}

/**
 * Render a preview report after a dry-run import.
 *
 * @param array $report
 *   The report array collected during the dry run.
 * @param bool $detailed
 *   TRUE to include item-level lists; FALSE for counts only.
 */
function _content_export_import_show_report(array $report, $detailed) {
  // Build an indented <ul> from a pre-sanitized array of strings.
  $ul = function(array $items) {
    return '<ul style="margin:.3em 0 0 1.5em"><li>' . implode('</li><li>', $items) . '</li></ul>';
  };

  // --- Taxonomy ---
  $vocab_created = count($report['vocabularies_created']);
  $vocab_existing = count($report['vocabularies_existing']);
  $total_terms_created = 0;
  $total_terms_existing = 0;
  foreach ($report['terms_created'] as $terms) {
    $total_terms_created += count($terms);
  }
  foreach ($report['terms_existing'] as $terms) {
    $total_terms_existing += count($terms);
  }
  $tax_msg = t(
    '@vc vocabularies would be created, @ve already exist. @tc terms would be created, @te already exist.',
    array(
      '@vc' => $vocab_created,
      '@ve' => $vocab_existing,
      '@tc' => $total_terms_created,
      '@te' => $total_terms_existing,
    )
  );
  if ($detailed) {
    if (!empty($report['vocabularies_created'])) {
      $tax_msg .= '<br>' . t('New vocabularies:') . $ul(array_map('check_plain', $report['vocabularies_created']));
    }
    foreach ($report['terms_created'] as $vocab => $terms) {
      $tax_msg .= '<br>' . t('New terms in %vocab:', array('%vocab' => $vocab)) . $ul(array_map('check_plain', $terms));
    }
  }
  backdrop_set_message($tax_msg);

  // --- Content types ---
  $types_created = count($report['types_created']);
  $types_existing = count($report['types_existing']);
  $type_msg = t(
    '@tc content types would be created, @te already exist.',
    array('@tc' => $types_created, '@te' => $types_existing)
  );
  if ($detailed && !empty($report['types_created'])) {
    $type_msg .= '<br>' . t('New types:') . $ul(array_map('check_plain', $report['types_created']));
  }
  backdrop_set_message($type_msg);

  // --- Nodes ---
  $nodes_import = count($report['nodes_to_import']);
  $nodes_skip   = count($report['nodes_skipped_uuid']);
  $nodes_dup    = count($report['nodes_duplicate_title']);

  $node_msg = t(
    '@ni nodes would be imported, @ns skipped (duplicate UUID).',
    array('@ni' => $nodes_import, '@ns' => $nodes_skip)
  );
  if ($detailed && !empty($report['nodes_to_import'])) {
    // Build the import list, annotating title conflicts inline.
    $dup_set = array_flip($report['nodes_duplicate_title']);
    $items = array();
    foreach ($report['nodes_to_import'] as $title) {
      $item = check_plain($title);
      if (isset($dup_set[$title])) {
        $item .= ' <em>(' . t('title conflict') . ')</em>';
      }
      $items[] = $item;
    }
    $node_msg .= '<br>' . t('Nodes to import:') . $ul($items);
  }
  if ($detailed && !empty($report['nodes_skipped_uuid'])) {
    $node_msg .= '<br>' . t('Nodes skipped (UUID exists):') . $ul(array_map('check_plain', $report['nodes_skipped_uuid']));
  }
  backdrop_set_message($node_msg);

  // Separate warning for title conflicts — clarifies they are still imported.
  if ($nodes_dup > 0) {
    $dup_msg = t(
      '@nd node(s) will be imported even though a node with the same title already exists on this site.',
      array('@nd' => $nodes_dup)
    );
    if ($detailed) {
      $dup_msg .= $ul(array_map('check_plain', $report['nodes_duplicate_title']));
    }
    backdrop_set_message($dup_msg, 'warning');
  }

  // --- Files and comments (counts only in both modes) ---
  backdrop_set_message(t('@count files would be imported.', array('@count' => $report['files_to_import'])));
  backdrop_set_message(t('@count comments would be imported.', array('@count' => $report['comments_to_import'])));
}

/**
 * Check if an entity with the given UUID already exists.
 *
 * @param string $entity_type
 *   The type of entity to check (e.g., 'node', 'file').
 * @param string $uuid
 *   The UUID to check for.
 *
 * @return bool
 *   TRUE if an entity with the UUID exists, FALSE otherwise.
 */
function content_export_import_entity_exists_by_uuid($entity_type, $uuid) {
  // If field_uuid doesn't exist yet, no entity can have a stored UUID.
  if (!field_info_field('field_uuid')) {
    return FALSE;
  }
  $query = new EntityFieldQuery();
  $query->entityCondition('entity_type', $entity_type)
        ->fieldCondition('field_uuid', 'value', $uuid);
  $result = $query->execute();
  return !empty($result[$entity_type]);
}
