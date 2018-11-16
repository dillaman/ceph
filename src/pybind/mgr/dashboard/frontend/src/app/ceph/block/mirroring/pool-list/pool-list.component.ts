import { Component, OnDestroy, OnInit, TemplateRef, ViewChild } from '@angular/core';
import { Subscription } from 'rxjs';

import { RbdMirroringService } from '../../../../shared/api/rbd-mirroring.service';
import { CdTableAction } from '../../../../shared/models/cd-table-action';
import { CdTableSelection } from '../../../../shared/models/cd-table-selection';
import { Permission } from '../../../../shared/models/permissions';
import { AuthStorageService } from '../../../../shared/services/auth-storage.service';

@Component({
  selector: 'cd-mirroring-pools',
  templateUrl: './pool-list.component.html',
  styleUrls: ['./pool-list.component.scss']
})
export class PoolListComponent implements OnInit, OnDestroy {
  @ViewChild('healthTmpl')
  healthTmpl: TemplateRef<any>;

  subs: Subscription;

  permission: Permission;
  tableActions: CdTableAction[];
  selection = new CdTableSelection();

  data: [];
  columns: {};

  constructor(
    private authStorageService: AuthStorageService,
    private rbdMirroringService: RbdMirroringService
  ) {
    this.permission = this.authStorageService.getPermissions().rbdMirroring;

    const editModeAction: CdTableAction = {
      permission: 'update',
      icon: 'fa-edit',
      click: () => this.editModeModal(),
      name: 'Edit Mode'
    };
    const addPeerAction: CdTableAction = {
      permission: 'create',
      icon: 'fa-plus',
      name: 'Add Peer',
      click: () => this.editPeersModal('add'),
      disable: (selection: CdTableSelection) => (
        !this.selection.first() || this.selection.first().mirror_mode == 'disabled'),
      visible: (selection: CdTableSelection) => true
    };
    const editPeerAction: CdTableAction = {
      permission: 'update',
      icon: 'fa-exchange',
      name: 'Edit Peer',
      click: () => this.editPeersModal('edit'),
      visible: (selection: CdTableSelection) => true
    };
    const deletePeerAction: CdTableAction = {
      permission: 'delete',
      icon: 'fa-times',
      name: 'Delete Peer',
      click: () => this.editPeersModal('delete'),
      visible: (selection: CdTableSelection) => true
    };
    this.tableActions = [
      editModeAction,
      addPeerAction,
      editPeerAction,
      deletePeerAction
    ];
  }

  ngOnInit() {
    this.columns = [
      { prop: 'name', name: 'Name', flexGrow: 2 },
      { prop: 'mirror_mode', name: 'Mode', flexGrow: 2 },
      { prop: 'leader_id', name: 'Leader', flexGrow: 2 },
      { prop: 'image_local_count', name: '# Local', flexGrow: 2 },
      { prop: 'image_remote_count', name: '# Remote', flexGrow: 2 },
      {
        prop: 'health',
        name: 'Health',
        cellTemplate: this.healthTmpl,
        flexGrow: 1
      }
    ];

    this.subs = this.rbdMirroringService.subscribe((data: any) => {
      if (!data) {
        return;
      }
      this.data = data.content_data.pools;
    });
  }

  ngOnDestroy(): void {
    this.subs.unsubscribe();
  }

  refresh() {
    this.rbdMirroringService.refresh();
  }

  editModeModal() {
  }

  editPeersModal(mode) {
  }

  updateSelection(selection: CdTableSelection) {
    this.selection = selection;
  }
}
