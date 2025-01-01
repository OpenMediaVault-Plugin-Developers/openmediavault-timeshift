# @license   http://www.gnu.org/licenses/gpl.html GPL Version 3
# @author    openmediavault plugin developers <plugins@omv-extras.org>
# @copyright Copyright (c) 2024-2025 openmediavault plugin developers
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.

{% set config = salt['omv_conf.get']('conf.service.timeshift') %}

{% set snapshot_count = "0" %}
{% set snapshot_size = "0" %}

{% set cfg_file = '/etc/timeshift/timeshift.json' %}
{% set exists = salt['file.file_exists'](cfg_file) %}

{% if exists %}

{% set cfg = salt.cp.get_file_str(cfg_file) | load_json %}

{% set ss_exists = 'snapshot_size' in cfg %}
{% set sc_exists = 'snapshot_count' in cfg %}

{% if ss_exists %}
  {% set snapshot_size = cfg['snapshot_size'] %}
{% endif %}

{% if sc_exists %}
  {% set snapshot_count = cfg['snapshot_count'] %}
{% endif %}

{% endif %}

configure_timeshift:
  file.managed:
    - name: "{{ cfg_file }}"
    - source:
      - salt://{{ tpldir }}/files/etc-timeshift-timeshift_json.j2
    - template: jinja
    - context:
        config: {{ config | json }}
        snapshot_count: {{ snapshot_count }}
        snapshot_size: {{ snapshot_size }}
    - user: root
    - group: root
    - mode: 644
