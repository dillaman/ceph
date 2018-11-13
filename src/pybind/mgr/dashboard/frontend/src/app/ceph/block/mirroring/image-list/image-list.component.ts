import { Component, OnDestroy, OnInit, TemplateRef, ViewChild } from '@angular/core';
import { Subscription } from 'rxjs';

import { RbdMirroringService } from '../../../../shared/api/rbd-mirroring.service';

import { DaemonListComponent } from '../daemon-list/daemon-list.component';
import { PoolListComponent } from '../pool-list/pool-list.component';

@Component({
  selector: 'cd-mirroring-images',
  templateUrl: './image-list.component.html',
  styleUrls: ['./image-list.component.scss']
})
export class ImageListComponent implements OnInit, OnDestroy {
  @ViewChild('stateTmpl')
  stateTmpl: TemplateRef<any>;
  @ViewChild('syncTmpl')
  syncTmpl: TemplateRef<any>;
  @ViewChild('progressTmpl')
  progressTmpl: TemplateRef<any>;

  subs: Subscription;

  daemons: DaemonListComponent;
  pools: PoolListComponent;
  image_error = {
    data: [],
    columns: {}
  };
  image_syncing = {
    data: [],
    columns: {}
  };
  image_ready = {
    data: [],
    columns: {}
  };

  constructor(private rbdMirroringService: RbdMirroringService) {}

  ngOnInit() {
    this.image_error.columns = [
      { prop: 'pool_name', name: 'Pool', flexGrow: 2 },
      { prop: 'name', name: 'Image', flexGrow: 2 },
      { prop: 'description', name: 'Issue', flexGrow: 4 },
      {
        prop: 'state',
        name: 'State',
        cellTemplate: this.stateTmpl,
        flexGrow: 1
      }
    ];

    this.image_syncing.columns = [
      { prop: 'pool_name', name: 'Pool', flexGrow: 2 },
      { prop: 'name', name: 'Image', flexGrow: 2 },
      {
        prop: 'progress',
        name: 'Progress',
        cellTemplate: this.progressTmpl,
        flexGrow: 2
      },
      {
        prop: 'state',
        name: 'State',
        cellTemplate: this.syncTmpl,
        flexGrow: 1
      }
    ];

    this.image_ready.columns = [
      { prop: 'pool_name', name: 'Pool', flexGrow: 2 },
      { prop: 'name', name: 'Image', flexGrow: 2 },
      { prop: 'description', name: 'Description', flexGrow: 4 },
      {
        prop: 'state',
        name: 'State',
        cellTemplate: this.stateTmpl,
        flexGrow: 1
      }
    ];

    this.subs = this.rbdMirroringService.subscribe((data: any) => {
      if (!data) {
        return;
      }
      this.image_error.data = data.content_data.image_error;
      this.image_syncing.data = data.content_data.image_syncing;
      this.image_ready.data = data.content_data.image_ready;
    });
  }

  ngOnDestroy(): void {
    this.subs.unsubscribe();
  }

  refresh() {
    this.rbdMirroringService.refresh();
  }
}
