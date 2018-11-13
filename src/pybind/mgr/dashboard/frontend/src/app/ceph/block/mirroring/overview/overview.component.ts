import { Component, OnDestroy, OnInit } from '@angular/core';
import { Subscription } from 'rxjs';

import { RbdMirroringService } from '../../../../shared/api/rbd-mirroring.service';
import { ViewCacheStatus } from '../../../../shared/enum/view-cache-status.enum';

import { DaemonListComponent } from '../daemon-list/daemon-list.component';
import { ImageListComponent } from '../image-list/image-list.component';
import { PoolListComponent } from '../pool-list/pool-list.component';

@Component({
  selector: 'cd-mirroring',
  templateUrl: './overview.component.html',
  styleUrls: ['./overview.component.scss']
})
export class OverviewComponent implements OnInit, OnDestroy {
  subs: Subscription;

  status: ViewCacheStatus;
  daemons: DaemonListComponent;
  pools: PoolListComponent;
  images: ImageListComponent;

  constructor(private rbdMirroringService: RbdMirroringService) {}

  ngOnInit() {
    this.subs = this.rbdMirroringService.subscribe((data: any) => {
      if (!data) {
        return;
      }
      this.status = data.content_data.status;
    });
  }

  ngOnDestroy(): void {
    this.subs.unsubscribe();
  }
}
