BEGIN {
  FS = ",";
}

NR > 1 {
  checkpoint_sum += $2;
  if (checkpoint_n == 0 || $2 < checkpoint_min) checkpoint_min = $2;
  if ($2 > checkpoint_max) checkpoint_max = $2;
  checkpoint_n++;

  restore_sum += $3;
  if (restore_n == 0 || $3 < restore_min) restore_min = $3;
  if ($3 > restore_max) restore_max = $3;
  restore_n++;

  snapshot_sum += $11;
  if (snapshot_n == 0 || $11 < snapshot_min) snapshot_min = $11;
  if ($11 > snapshot_max) snapshot_max = $11;
  snapshot_n++;
}

END {
  printf "checkpoint %.3f %.3f %.3f n=%d\n", checkpoint_sum / checkpoint_n, checkpoint_min, checkpoint_max, checkpoint_n;
  printf "restore %.3f %.3f %.3f n=%d\n", restore_sum / restore_n, restore_min, restore_max, restore_n;
  printf "snapshot %.2f %d %d n=%d\n", snapshot_sum / snapshot_n, snapshot_min, snapshot_max, snapshot_n;
}
